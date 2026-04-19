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

    Recorder rec;
    if (opts.record_path) {
        auto initial = sessions[0]->snapshot_for(0);
        rec.begin(kHz, opts.num_players, opts.seed,
                  /*world_capacity=*/256,
                  {initial.data(), initial.size()});
    }

    std::uint64_t last_print_tick = 0;

    for (std::uint32_t f = 0; f < opts.frames; ++f) {
        // Each player ticks with their own AI input.
        for (std::uint8_t p = 0; p < opts.num_players; ++p) {
            sessions[p]->tick(ai_input(f, p));
        }
        hub.advance_tick();

        // Recorder uses player 0's view of the per-player inputs.
        if (opts.record_path) {
            std::vector<PlayerInput> ppi(opts.num_players);
            for (std::uint8_t p = 0; p < opts.num_players; ++p) {
                ppi[p] = ai_input(f, p);
            }
            rec.record(f, {ppi.data(), ppi.size()},
                       sessions[0]->stats().last_state_hash);
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
        auto bytes = rec.finish(sessions[0]->stats().last_state_hash);
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
