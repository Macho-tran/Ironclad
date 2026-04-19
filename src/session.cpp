// SPDX-License-Identifier: MIT
#include <ironclad/session.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

#include <ironclad/byteio.hpp>
#include <ironclad/packet.hpp>

namespace ironclad {

namespace {

constexpr std::uint16_t kInputRing = 256;     // power-of-two

inline std::size_t input_idx(std::uint32_t frame) noexcept {
    return frame & (kInputRing - 1u);
}

}  // namespace

Session::Session(SessionConfig    config,
                 std::unique_ptr<ITransport> transport,
                 WorldInit        world_init,
                 SimStep          step)
    : config_(config),
      transport_(std::move(transport)),
      step_(std::move(step)),
      world_(config.world_capacity),
      rng_(config.seed),
      snapshots_(config.snapshot_ring),
      inputs_(config.num_players,
              std::vector<PlayerInput>(kInputRing)),
      input_valid_(config.num_players,
                   std::vector<std::uint8_t>(kInputRing, 0)),
      last_known_input_frame_(config.num_players, 0)
{
    // The world initializer is responsible for registering the
    // component types in a fixed order on every peer (component order
    // determines snapshot byte layout).
    world_init(world_, rng_, config_);

    // Snapshot frame 0 (the initial state) so rollback can reach it.
    save_snapshot(0);
    stats_.current_frame  = 0;
    stats_.last_state_hash = last_hash_;
}

void Session::tick(PlayerInput local_input) {
    transport_->poll();

    // 1) Record the local input for this frame *before* any rollback,
    //    so a replay always sees the same local input it originally
    //    saw.
    inputs_[config_.local_player][input_idx(frame_)]      = local_input;
    input_valid_[config_.local_player][input_idx(frame_)] = 1;
    last_known_input_frame_[config_.local_player] =
        std::max(last_known_input_frame_[config_.local_player], frame_);

    // 2) Drain transport: process remote input packets, possibly
    //    triggering rollback.
    drain_transport();

    // 3) Step the simulation one frame.
    step_one(frame_);
    save_snapshot(frame_ + 1);

    // 4) Bookkeeping.
    ++frame_;
    stats_.current_frame  = frame_;
    stats_.last_state_hash = last_hash_;
    ++stats_.total_ticks;

    // 5) Broadcast input packet to all peers.
    broadcast_input_packet();
}

void Session::step_one(std::uint32_t frame) {
    // Assemble inputs for every player for `frame`.
    std::vector<PlayerInput> per_player(config_.num_players);
    for (std::uint8_t p = 0; p < config_.num_players; ++p) {
        per_player[p] = input_for_step(p, frame);
    }

    // Run the user-supplied step function.
    step_(world_, per_player.data(), config_.num_players, rng_);
}

PlayerInput Session::input_for_step(std::uint8_t player,
                                    std::uint32_t frame) const {
    if (input_valid_[player][input_idx(frame)] != 0u) {
        return inputs_[player][input_idx(frame)];
    }
    // Predict: repeat the last known input for this player.
    std::uint32_t last_frame = last_known_input_frame_[player];
    if (input_valid_[player][input_idx(last_frame)] != 0u) {
        return inputs_[player][input_idx(last_frame)];
    }
    return PlayerInput{};
}

void Session::save_snapshot(std::uint32_t frame) {
    ByteWriter w;
    world_.serialize(w);
    // Append RNG state to the bytes-being-hashed so the snapshot bytes
    // capture the entire authoritative simulation state.
    w.write_u64(rng_.state());
    last_hash_ = hash64(w.view().data(), w.view().size());
    auto bytes = w.take();
    snapshots_.store(frame, last_hash_, std::move(bytes));
}

bool Session::load_snapshot(std::uint32_t frame) {
    const Snapshot* s = snapshots_.get(frame);
    if (!s) return false;
    ByteReader r(s->bytes.data(), s->bytes.size());
    if (!world_.deserialize(r)) return false;
    // Read appended RNG state.
    auto rng_state = r.read_u64();
    if (r.error()) return false;
    rng_.set_state(rng_state);
    last_hash_ = s->hash;
    return true;
}

std::uint32_t Session::process_remote_input(std::uint8_t player,
                                            std::uint32_t frame,
                                            PlayerInput in) {
    if (player == config_.local_player) return ~std::uint32_t{0};
    if (frame >= frame_)               {
        // Future input; just store it. Predictions from the past
        // don't apply yet, so no rollback.
        inputs_[player][input_idx(frame)]      = in;
        input_valid_[player][input_idx(frame)] = 1;
        last_known_input_frame_[player] =
            std::max(last_known_input_frame_[player], frame);
        return ~std::uint32_t{0};
    }
    // Past frame: compare against what we predicted/used during the
    // earlier step. We must capture the predicted value *before*
    // overwriting the slot, otherwise we'd compare an input to
    // itself and never detect divergence.
    const PlayerInput predicted = input_for_step(player, frame);
    inputs_[player][input_idx(frame)]      = in;
    input_valid_[player][input_idx(frame)] = 1;
    last_known_input_frame_[player] =
        std::max(last_known_input_frame_[player], frame);
    return predicted != in ? frame : ~std::uint32_t{0};
}

void Session::drain_transport() {
    std::uint32_t rollback_to = ~std::uint32_t{0};
    while (auto pkt = transport_->recv()) {
        ByteReader r(pkt->bytes.data(), pkt->bytes.size());
        InputPacket ip;
        if (!read_input_packet(r, ip)) continue;
        if (ip.sender != pkt->from_player) continue;       // sanity
        if (ip.sender >= config_.num_players) continue;
        // Newest input is at index `count - 1`, frame `ip.frame`.
        for (std::uint8_t i = 0; i < ip.count; ++i) {
            const std::uint32_t f = ip.frame + 1u - ip.count + i;
            const std::uint32_t r_to =
                process_remote_input(ip.sender, f, ip.inputs[i]);
            if (r_to < rollback_to) rollback_to = r_to;
        }
        // Desync detection: if peer reports a state hash for a frame
        // we've already produced, compare it to our own.
        if (ip.ack_frame > 0 && ip.ack_frame <= frame_) {
            const Snapshot* mine = snapshots_.get(ip.ack_frame);
            if (mine && mine->hash != ip.ack_hash) {
                stats_.desync_detected     = true;
                stats_.desync_frame        = ip.ack_frame;
                stats_.desync_local_hash   = mine->hash;
                stats_.desync_remote_hash  = ip.ack_hash;
            }
        }
        // Track the latest peer hash we've now verified, for future
        // outbound packets.
        if (ip.ack_frame >= ack_frame_) {
            ack_frame_ = ip.ack_frame;
            ack_hash_  = ip.ack_hash;
        }
    }

    if (rollback_to != ~std::uint32_t{0}) {
        // Restore world to the snapshot taken *before* the divergent frame.
        if (load_snapshot(rollback_to)) {
            const std::uint32_t old_frame = frame_;
            for (std::uint32_t f = rollback_to; f < old_frame; ++f) {
                step_one(f);
                save_snapshot(f + 1);
            }
            const std::uint32_t rolled = old_frame - rollback_to;
            stats_.last_rollback_frames =
                static_cast<std::uint8_t>(std::min<std::uint32_t>(rolled, 255u));
            stats_.total_rollback_frames += rolled;
        }
    } else {
        stats_.last_rollback_frames = 0;
    }
}

void Session::broadcast_input_packet() {
    InputPacket ip;
    ip.sender    = config_.local_player;
    ip.frame     = frame_ - 1;     // most recent fully-stepped frame
    ip.ack_frame = ack_frame_;
    ip.ack_hash  = ack_hash_;
    const std::uint8_t want = std::min<std::uint8_t>(InputPacket::kMaxInputs,
        static_cast<std::uint8_t>(std::min<std::uint32_t>(frame_, 255u)));
    ip.count = want;
    for (std::uint8_t i = 0; i < want; ++i) {
        const std::uint32_t f = ip.frame + 1u - want + i;
        ip.inputs[i] = inputs_[config_.local_player][input_idx(f)];
    }
    ByteWriter w;
    write_input_packet(w, ip);
    stats_.bytes_sent += w.size() * (config_.num_players - 1u);
    for (std::uint8_t p = 0; p < config_.num_players; ++p) {
        if (p == config_.local_player) continue;
        transport_->send(p, w.view());
    }
}

std::optional<PlayerInput>
Session::input_for(std::uint8_t player, std::uint32_t frame) const {
    if (player >= config_.num_players) return std::nullopt;
    if (input_valid_[player][input_idx(frame)] == 0u) return std::nullopt;
    return inputs_[player][input_idx(frame)];
}

std::span<const std::uint8_t>
Session::snapshot_for(std::uint32_t frame) const {
    const Snapshot* s = snapshots_.get(frame);
    if (!s) return {};
    return {s->bytes.data(), s->bytes.size()};
}

bool Session::fully_acked(std::uint32_t frame) const {
    for (std::uint8_t p = 0; p < config_.num_players; ++p) {
        if (input_valid_[p][input_idx(frame)] == 0u) return false;
    }
    return true;
}

}  // namespace ironclad
