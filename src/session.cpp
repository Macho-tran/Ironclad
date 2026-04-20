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

    // Pre-fill the input-delay window for the local player with
    // valid empty inputs. Without this, slots [0..delay-1] of the
    // local player are never written (the first `tick(input)` call
    // writes slot `delay`), and `fully_acked()` reports false for
    // those frames forever — which in turn prevents honest cross-
    // attestation of early-game state hashes.
    for (std::uint8_t f = 0; f < config_.local_input_delay; ++f) {
        inputs_[config_.local_player][input_idx(f)]      = PlayerInput{};
        input_valid_[config_.local_player][input_idx(f)] = 1;
    }
    if (config_.local_input_delay > 0) {
        last_known_input_frame_[config_.local_player] =
            static_cast<std::uint32_t>(config_.local_input_delay - 1);
    }

    // Snapshot frame 0 (the initial state) so rollback can reach it.
    save_snapshot(0);
    stats_.current_frame  = 0;
    stats_.last_state_hash = last_hash_;
}

void Session::tick(PlayerInput local_input) {
    transport_->poll();

    // 1) Record the local input for the *future* frame `frame_ + delay`.
    //    This is the input-delay trick: pressing a button now causes
    //    the action to occur `delay` ticks from now, which gives every
    //    remote peer that many ticks of grace before they have to
    //    predict our input. Smaller average rollback at the cost of
    //    a few ms of perceived input lag (default 2 ticks ≈ 33 ms).
    const std::uint32_t apply_at =
        frame_ + static_cast<std::uint32_t>(config_.local_input_delay);
    inputs_[config_.local_player][input_idx(apply_at)]      = local_input;
    input_valid_[config_.local_player][input_idx(apply_at)] = 1;
    last_known_input_frame_[config_.local_player] =
        std::max(last_known_input_frame_[config_.local_player], apply_at);

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
    // First choice: an authoritative input recorded for exactly this
    // frame.
    if (input_valid_[player][input_idx(frame)] != 0u) {
        return inputs_[player][input_idx(frame)];
    }
    // Otherwise: predict by repeating the most recent valid input
    // *at or before* `frame`. Walking the ring backward is cheap
    // (typically 1-2 steps) and keeps us from accidentally using a
    // future input as the predicted past — a subtle bug that would
    // both defeat the local input-delay trick and inflate average
    // rollback distance for remote peers.
    if (frame == 0) return PlayerInput{};
    const std::uint32_t window = std::min<std::uint32_t>(kInputRing, frame);
    for (std::uint32_t back = 1; back <= window; ++back) {
        const std::uint32_t f = frame - back;
        if (input_valid_[player][input_idx(f)] != 0u) {
            return inputs_[player][input_idx(f)];
        }
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
    // Collect peer attestations from this drain pass; we evaluate
    // them only AFTER any rollback has updated our snapshots, so a
    // packet that simultaneously delivers corrective input and the
    // peer's hash can't false-positive (we'd otherwise compare the
    // peer's post-correction hash against our stale prediction).
    struct PeerAttest { std::uint32_t ack_frame; std::uint64_t ack_hash; };
    std::vector<PeerAttest> attestations;

    while (auto pkt = transport_->recv()) {
        ByteReader r(pkt->bytes.data(), pkt->bytes.size());
        InputPacket ip;
        if (!read_input_packet(r, ip)) continue;
        if (ip.sender != pkt->from_player) continue;       // sanity
        if (ip.sender >= config_.num_players) continue;
        for (std::uint8_t i = 0; i < ip.count; ++i) {
            const std::uint32_t f = ip.frame + 1u - ip.count + i;
            const std::uint32_t r_to =
                process_remote_input(ip.sender, f, ip.inputs[i]);
            if (r_to < rollback_to) rollback_to = r_to;
        }
        if (ip.ack_frame > 0) {
            attestations.push_back({ip.ack_frame, ip.ack_hash});
        }
    }

    if (rollback_to != ~std::uint32_t{0}) {
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

    // Now that any corrective rollback has executed, evaluate the
    // peer attestations. We only act on attestations for frames we
    // ourselves have fully acked from every peer — otherwise our
    // snapshot is still mutable and a comparison is a race.
    for (const auto& a : attestations) {
        if (a.ack_frame > frame_) continue;
        if (!fully_acked(a.ack_frame - 1)) continue;
        const Snapshot* mine = snapshots_.get(a.ack_frame);
        if (mine && mine->hash != a.ack_hash) {
            stats_.desync_detected     = true;
            stats_.desync_frame        = a.ack_frame;
            stats_.desync_local_hash   = mine->hash;
            stats_.desync_remote_hash  = a.ack_hash;
        }
    }
}

void Session::broadcast_input_packet() {
    InputPacket ip;
    ip.sender    = config_.local_player;
    ip.frame     = frame_ - 1;     // most recent fully-stepped frame
    // Attest to our own state hash for the most recent frame that
    // is BOTH fully acked AND old enough that no in-flight packet
    // could still trigger a rollback through it.
    constexpr std::uint32_t kDesyncMargin = 30;     // ~0.5s @ 60Hz
    ip.ack_frame = 0;
    ip.ack_hash  = 0;
    const std::uint32_t cap = frame_ > kDesyncMargin
        ? frame_ - kDesyncMargin : 0;
    if (cap > 0) {
        for (std::uint32_t f = cap; f-- > 0;) {
            if (!fully_acked(f)) continue;
            const Snapshot* s = snapshots_.get(f + 1);
            if (!s) continue;
            ip.ack_frame = f + 1;
            ip.ack_hash  = s->hash;
            break;
        }
    }
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
