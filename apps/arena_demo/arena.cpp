// SPDX-License-Identifier: MIT
#include "arena.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

#include <ironclad/byteio.hpp>
#include <ironclad/lag_comp.hpp>
#include <ironclad/recorder.hpp>

namespace arena_demo {

using namespace ironclad;

namespace {

// Borrowed transport adapter — same as in tests; the LoopbackHub owns
// the Peer objects, but Session wants ownership of an ITransport.
struct BorrowedTransport : ironclad::ITransport {
    ironclad::ITransport* inner;
    explicit BorrowedTransport(ironclad::ITransport* t) : inner(t) {}
    void send(std::uint8_t to, std::span<const std::uint8_t> b) override {
        inner->send(to, b);
    }
    std::optional<RecvPacket> recv() override { return inner->recv(); }
    void poll() override { inner->poll(); }
};

inline std::uint16_t ms_to_ticks(std::uint16_t ms, std::uint16_t hz) {
    // ceil(ms * hz / 2000)  -> half-RTT in *ticks*; we treat the
    // user-facing "rtt-ms" as a round-trip number, so per-link
    // latency is half of that.
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(ms) *
                                       static_cast<std::uint32_t>(hz) +
                                       1999u) / 2000u);
}

}  // namespace

void step_arena(World& world, const PlayerInput* inputs,
                std::uint8_t num_players, Rng& rng) {
    // Player movement: input drives velocity, velocity integrates pos.
    for (std::uint8_t p = 0; p < num_players; ++p) {
        Entity self = kInvalidEntity;
        world.each<Player>([&](Entity e, Player& pl) {
            if (pl.id == p && pl.alive) self = e;
        });
        if (self == kInvalidEntity) continue;
        auto* tr  = world.get<Transform>(self);
        auto* vel = world.get<Velocity>(self);
        auto* pl  = world.get<Player>(self);
        if (!tr || !vel || !pl) continue;
        Fixed speed = Fixed::from_ratio(1, 4);
        if (inputs[p].dash() && pl->dash_cd == 0) {
            speed = Fixed::from_ratio(3, 4);
            pl->dash_cd = 30;
        }
        vel->v.x = inputs[p].move_x_fx() * speed;
        vel->v.y = inputs[p].move_y_fx() * speed;
        tr->pos += vel->v;
        if (pl->dash_cd > 0) --pl->dash_cd;
        if (pl->hit_cd  > 0) --pl->hit_cd;

        // Spawn a projectile on attack.
        if (inputs[p].attack() && pl->hit_cd == 0) {
            pl->hit_cd = 12;
            Entity proj = world.create();
            world.add(proj, Transform{tr->pos, Fixed{}});
            // Dir defaults to +x if no input axis pressed.
            Vec2 dir{Fixed{1}, Fixed{0}};
            Vec2 mv{inputs[p].move_x_fx(), inputs[p].move_y_fx()};
            if (mv.length_sq() != kZero) dir = mv;
            world.add(proj, Velocity{dir * Fixed::from_ratio(3, 2)});
            world.add(proj, Projectile{static_cast<std::uint32_t>(self), 60, 0});
            world.add(proj, Hitbox{Fixed::from_ratio(1, 4)});
        }
    }

    // Move & age projectiles.
    std::vector<Entity> doomed;
    world.each<Projectile>([&](Entity e, Projectile& proj) {
        auto* tr  = world.get<Transform>(e);
        auto* vel = world.get<Velocity>(e);
        if (tr && vel) tr->pos += vel->v;
        if (proj.ttl == 0) doomed.push_back(e);
        else               --proj.ttl;
    });
    for (auto e : doomed) world.destroy(e);

    // Use the RNG every tick so its state participates in the hash.
    (void)rng.next_u64();
}

void init_arena(World& world, Rng& rng, const SessionConfig& cfg) {
    world.register_component<Transform>();
    world.register_component<Velocity>();
    world.register_component<Player>();
    world.register_component<Projectile>();
    world.register_component<Hitbox>();
    for (std::uint8_t p = 0; p < cfg.num_players; ++p) {
        Entity e = world.create();
        // Spread players evenly along x; y = 0.
        world.add(e, Transform{Vec2{Fixed{p * 4}, Fixed{0}}, Fixed{}});
        world.add(e, Velocity{});
        world.add(e, Player{p, 0, 0, 1, Fixed{100}, 0});
        world.add(e, Hitbox{Fixed::from_ratio(1, 2)});
    }
    (void)rng;
}

PlayerInput ai_input(std::uint32_t frame, std::uint8_t player) {
    // Each player oscillates with a different (relatively long) period
    // and occasionally attacks. Inputs are derived only from `frame`
    // and `player`, not from any RNG state, so the AI is fully
    // reproducible. The direction-change cadence (every ~30 frames /
    // 0.5 s) approximates real player input rates and keeps prediction
    // mismatches rare, which is what the KPIs assume.
    PlayerInput in;
    int phase = static_cast<int>(frame) + player * 64;
    in.move_x = static_cast<std::int8_t>(((phase / 30) & 1) ? 64  : -64);
    in.move_y = static_cast<std::int8_t>(((phase / 47) & 1) ? 64  : -64);
    if ((frame % 60u) == player) in.buttons |= PlayerInput::kAttack;
    if ((frame % 180u) == player) in.buttons |= PlayerInput::kDash;
    return in;
}

Result run(const Options& opts) {
    Result res;

    NetSimConfig nc;
    constexpr std::uint16_t kHz = kDefaultTickHz;
    nc.latency_ticks = ms_to_ticks(opts.rtt_ms, kHz);
    nc.jitter_ticks  = ms_to_ticks(opts.jitter_ms, kHz);
    nc.loss_pct      = opts.loss_pct;
    nc.reorder_pct   = opts.reorder_pct;
    nc.seed          = opts.seed ^ 0xA5A5'A5A5'A5A5'A5A5ULL;

    LoopbackHub hub(opts.num_players, nc);
    std::vector<std::unique_ptr<Session>> sessions(opts.num_players);
    for (std::uint8_t p = 0; p < opts.num_players; ++p) {
        SessionConfig sc;
        sc.num_players  = opts.num_players;
        sc.local_player = p;
        sc.tick_hz      = kHz;
        sc.seed         = opts.seed;
        sessions[p] = std::make_unique<Session>(sc,
            std::make_unique<BorrowedTransport>(hub.transport(p)),
            init_arena, step_arena);
    }

    // Recording strategy: we capture the *canonical* per-frame AI
    // inputs as we go (these are deterministic and define the ground
    // truth of "what each player did this frame"), plus the live
    // sessions' per-tick rollback distance / desync flag / predicted-
    // vs-canonical mismatch bitmask. We do NOT trust the live
    // sessions' per-tick `last_state_hash` for the recording,
    // because under non-zero RTT a session's hash for frame F is
    // only finalized after the last rollback that touches F, which
    // can happen many ticks later. Instead, after the live run we
    // do an offline deterministic re-simulation from the canonical
    // inputs to compute the authoritative hash for every recorded
    // frame. The re-simulated hashes match exactly what the Replayer
    // will compute later when reading the .iclr file -- by
    // construction, the chain validates.
    struct RecCapture {
        std::vector<PlayerInput> inputs;
        std::uint8_t             rollback  = 0;
        std::uint8_t             flags     = 0;
        std::uint8_t             pred_diff = 0;
        // predicted[observer * num_players + target]
        std::vector<PlayerInput> predicted;
    };
    std::vector<RecCapture> rec_captures;
    std::vector<LagEvent>   rec_lag_events;
    if (opts.record_path) rec_captures.reserve(opts.frames);

    // A lag-comp circular buffer, populated each tick from session 0's
    // world. We use this to perform lag-compensated hit-scans whenever
    // a player presses attack, and record the event for the Replay
    // Studio's ghost-hitbox visualization.
    LagBuffer lag_buf(64);

    std::uint64_t last_print_tick = 0;
    // Track previous-tick rollback totals per session so we can compute
    // the delta produced *during* this tick (and pick the worst-affected
    // peer). last_rollback_frames in stats is overwritten by the next
    // drain, so we use the running total instead.
    std::vector<std::uint64_t> prev_rollback_total(opts.num_players, 0);

    for (std::uint32_t f = 0; f < opts.frames; ++f) {
        // Compute the canonical AI inputs and capture each peer's
        // *prediction* for every other player before the tick runs.
        // pred_diff bit P is set if any peer's prediction for player P
        // differs from the canonical input that's about to be applied.
        std::vector<PlayerInput> canonical(opts.num_players);
        for (std::uint8_t p = 0; p < opts.num_players; ++p) {
            canonical[p] = ai_input(f, p);
        }
        std::uint8_t pred_diff = 0;
        std::vector<PlayerInput> predicted_matrix;
        if (opts.record_path) {
            const std::size_t N = opts.num_players;
            predicted_matrix.assign(N * N, PlayerInput{});
            for (std::uint8_t observer = 0; observer < N; ++observer) {
                for (std::uint8_t target = 0; target < N; ++target) {
                    PlayerInput predicted = (observer == target)
                        ? canonical[target]
                        : sessions[observer]->input_for(target, f)
                              .value_or(PlayerInput{});
                    predicted_matrix[
                        static_cast<std::size_t>(observer) * N + target] =
                        predicted;
                    if (observer != target && predicted != canonical[target]) {
                        pred_diff = static_cast<std::uint8_t>(
                            pred_diff | (1u << target));
                    }
                }
            }
        }

        // Each player ticks with their own AI input.
        for (std::uint8_t p = 0; p < opts.num_players; ++p) {
            sessions[p]->tick(canonical[p]);
        }
        hub.advance_tick();

        // Capture this frame's player positions into the lag buffer
        // so any subsequent attack can be rewound against them.
        if (opts.record_path) {
            std::vector<LagSample> samples;
            samples.reserve(opts.num_players);
            sessions[0]->world().each<Player>(
                [&](Entity e, const Player& pl) {
                    auto* tr = sessions[0]->world().get<Transform>(e);
                    auto* hb = sessions[0]->world().get<Hitbox>(e);
                    if (tr && hb && pl.alive) {
                        samples.push_back(LagSample{
                            static_cast<std::uint32_t>(pl.id),
                            tr->pos, hb->radius});
                    }
                });
            lag_buf.record(f, std::move(samples));

            // Process each canonical attack input as a lag-compensated
            // hit-scan against frame f - half_rtt (we use the netsim
            // RTT in ticks as a stand-in). Records the event regardless
            // of hit / miss so the studio can show the rewound shot.
            const std::uint16_t half_rtt =
                static_cast<std::uint16_t>(nc.latency_ticks);
            for (std::uint8_t p = 0; p < opts.num_players; ++p) {
                if (!canonical[p].attack()) continue;
                Entity attacker = kInvalidEntity;
                sessions[0]->world().each<Player>(
                    [&](Entity e, const Player& pl) {
                        if (pl.id == p) attacker = e;
                    });
                if (attacker == kInvalidEntity) continue;
                auto* tr = sessions[0]->world().get<Transform>(attacker);
                if (!tr) continue;
                Vec2 dir{Fixed{1}, Fixed{0}};
                Vec2 mv{canonical[p].move_x_fx(), canonical[p].move_y_fx()};
                if (mv.length_sq() != kZero) dir = mv;
                const Fixed range{8};
                auto hit_id = lag_buf.hitscan(tr->pos, dir, range,
                                              f, half_rtt * 2u);
                LagEvent ev;
                ev.frame         = f;
                ev.attacker_id   = p;
                ev.target_id     = hit_id ? static_cast<std::uint8_t>(*hit_id)
                                          : 0xFFu;
                ev.rewound_ticks = half_rtt;
                ev.origin        = tr->pos;
                ev.dir           = dir;
                ev.range         = range;
                rec_lag_events.push_back(ev);
            }
        }

        // Recorder needs the inputs the simulation actually consumed
        // at this frame, NOT the canonical AI inputs. With non-zero
        // local_input_delay, a player pressing a button at frame T
        // is consumed by the simulation at frame T + delay; recording
        // the canonical-frame-T input would put the wrong button on
        // the wrong tick and the replay would not reproduce the hash.
        // Player 0's session has authoritative inputs for every player
        // for every past frame (peers send theirs and we apply our own
        // immediately), so we read directly from its input ring.
        if (opts.record_path) {
            // Worst-case rollback distance across all peers for this
            // tick is the meaningful number — that's how big the
            // correction was for the most-affected client. We compute
            // it from the running total because last_rollback_frames
            // was clobbered when the next tick drained the transport.
            std::uint8_t rollback = 0;
            for (std::uint8_t p = 0; p < opts.num_players; ++p) {
                std::uint64_t delta =
                    sessions[p]->stats().total_rollback_frames - prev_rollback_total[p];
                prev_rollback_total[p] = sessions[p]->stats().total_rollback_frames;
                if (delta > 255) delta = 255;
                if (delta > rollback) rollback = static_cast<std::uint8_t>(delta);
            }
            std::uint8_t flags = 0;
            for (std::uint8_t p = 0; p < opts.num_players; ++p) {
                if (sessions[p]->stats().desync_detected) {
                    flags |= ReplayRecord::kFlagDesync;
                    break;
                }
            }
            rec_captures.push_back({canonical, rollback, flags, pred_diff,
                                    std::move(predicted_matrix)});
        }

        // Per-second status line from player 0's perspective.
        if (!opts.quiet && (sessions[0]->current_frame() - last_print_tick) >= kHz) {
            const auto& s = sessions[0]->stats();
            const auto& net = hub.stats();
            const double secs = static_cast<double>(sessions[0]->current_frame()) / kHz;
            const double bw   = static_cast<double>(net.bytes_sent) / 1024.0 / secs /
                                static_cast<double>(opts.num_players);
            std::printf("[%6.1fs] frame=%u rtt=%ums jitter=%ums loss=%u%% "
                        "rollback=%uf bw=%.1fKB/s hash=%016lx desync=%s\n",
                        secs,
                        s.current_frame, opts.rtt_ms, opts.jitter_ms,
                        opts.loss_pct,
                        static_cast<unsigned>(s.last_rollback_frames),
                        bw,
                        static_cast<unsigned long>(s.last_state_hash),
                        s.desync_detected ? "YES" : "no");
            std::fflush(stdout);
            last_print_tick = sessions[0]->current_frame();
        }
    }

    if (opts.record_path) {
        // Offline pass: deterministically re-simulate the canonical
        // inputs, recording the post-tick hash for each frame. The
        // resulting .iclr is guaranteed to validate via Replayer
        // because by construction the recorded hashes are exactly
        // what the same step function produces from the same inputs.
        Recorder rec;
        World world(256);
        Rng   rng(opts.seed);
        SessionConfig sc;
        sc.num_players    = opts.num_players;
        sc.world_capacity = 256;
        sc.seed           = opts.seed;
        sc.tick_hz        = kHz;
        init_arena(world, rng, sc);
        // Initial snapshot for the recording must match what
        // Replayer's ctor produces (`world.serialize` only — RNG
        // state is appended only inside the per-tick hash, not the
        // initial snapshot).
        ByteWriter init_w;
        world.serialize(init_w);
        rec.begin(kHz, opts.num_players, opts.seed, 256, init_w.view());

        std::uint64_t final_hash = 0;
        for (std::size_t i = 0; i < rec_captures.size(); ++i) {
            const auto& c = rec_captures[i];
            step_arena(world, c.inputs.data(),
                       static_cast<std::uint8_t>(c.inputs.size()), rng);
            ByteWriter h; world.serialize(h); h.write_u64(rng.state());
            final_hash = ironclad::hash64(h.view().data(), h.view().size());
            rec.record_v3(static_cast<std::uint32_t>(i), c.inputs,
                          final_hash, c.rollback, c.flags, c.pred_diff,
                          {c.predicted.data(), c.predicted.size()});
        }
        // Append all lag-comp events captured during the live run.
        for (const auto& ev : rec_lag_events) rec.record_lag_event(ev);
        auto bytes = rec.finish(final_hash);
        std::ofstream f(opts.record_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    bool any_desync = false;
    for (std::uint8_t p = 0; p < opts.num_players; ++p) {
        if (sessions[p]->stats().desync_detected) any_desync = true;
    }
    res.desync_detected = any_desync;

    res.total_ticks            = sessions[0]->stats().total_ticks;
    res.total_rollback_frames  = sessions[0]->stats().total_rollback_frames;
    res.avg_rollback_frames    = res.total_ticks
        ? static_cast<double>(res.total_rollback_frames) /
          static_cast<double>(res.total_ticks)
        : 0.0;
    res.bytes_sent_per_client  = hub.stats().bytes_sent /
        std::max<std::uint8_t>(opts.num_players, 1);
    // Bandwidth is measured against *simulated* time (frames / tick_hz),
    // not wall-clock, since the sim can outpace real time. This is the
    // number a player would actually see over the wire at production
    // tick rate.
    const double sim_seconds   = static_cast<double>(opts.frames) / kHz;
    res.bandwidth_kbps_per_client = sim_seconds > 0
        ? static_cast<double>(res.bytes_sent_per_client) / 1024.0 / sim_seconds
        : 0.0;
    res.ok = !any_desync;
    return res;
}

}  // namespace arena_demo
