// SPDX-License-Identifier: MIT
//
// End-to-end test for the rollback Session.
//
// Two sessions, connected by a `LoopbackHub`. The simulation step is
// deliberately tiny but non-trivial: each player has Transform +
// Velocity, the velocity comes from input, and position integrates
// from velocity. The session RNG is touched every tick so its state
// is part of the snapshot hash.
//
// We verify two properties:
//   1. Two peers with identical scripted local inputs end up with
//      byte-identical snapshots, even after rollback.
//   2. Average rollback distance under simulated 150 ms RTT / 5 %
//      loss is bounded.
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <ironclad/components.hpp>
#include <ironclad/loopback_transport.hpp>
#include <ironclad/session.hpp>

using namespace ironclad;

namespace {

void step_arena(World& world, const PlayerInput* inputs,
                std::uint8_t num_players, Rng& rng) {
    for (std::uint8_t p = 0; p < num_players; ++p) {
        Entity self = kInvalidEntity;
        world.each<Player>([&](Entity e, Player& pl) {
            if (pl.id == p) self = e;
        });
        if (self == kInvalidEntity) continue;
        auto* tr  = world.get<Transform>(self);
        auto* vel = world.get<Velocity>(self);
        if (!tr || !vel) continue;
        vel->v.x = inputs[p].move_x_fx() * Fixed::from_ratio(1, 2);
        vel->v.y = inputs[p].move_y_fx() * Fixed::from_ratio(1, 2);
        tr->pos += vel->v;
    }
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
        world.add(e, Transform{Vec2{Fixed{p * 4}, Fixed{0}}, Fixed{}});
        world.add(e, Velocity{});
        world.add(e, Player{p, 0, 0, 1, Fixed{100}, 0});
        world.add(e, Hitbox{Fixed::from_ratio(1, 2)});
    }
    (void)rng;
}

PlayerInput ai_input(std::uint32_t frame, std::uint8_t player) {
    PlayerInput in;
    int phase = static_cast<int>(frame) + player * 16;
    in.move_x = static_cast<std::int8_t>(((phase / 7)  & 1) ? 64  : -64);
    in.move_y = static_cast<std::int8_t>(((phase / 11) & 1) ? 64  : -64);
    if ((frame % 30u) == player) in.buttons |= PlayerInput::kAttack;
    return in;
}

// LoopbackHub owns the per-peer transport objects; this adapter lets a
// Session "borrow" one without taking ownership.
struct BorrowedTransport : ironclad::ITransport {
    ironclad::ITransport* inner;
    explicit BorrowedTransport(ironclad::ITransport* t) : inner(t) {}
    void send(std::uint8_t to, std::span<const std::uint8_t> b) override {
        inner->send(to, b);
    }
    std::optional<RecvPacket> recv() override { return inner->recv(); }
    void poll() override { inner->poll(); }
};

struct TwoSession {
    LoopbackHub               hub;
    std::unique_ptr<Session>  a;
    std::unique_ptr<Session>  b;

    explicit TwoSession(NetSimConfig nc = {}, std::uint64_t seed = 0xBEEFFEED)
        : hub(2, nc) {
        SessionConfig ca;
        ca.num_players = 2; ca.local_player = 0; ca.seed = seed;
        SessionConfig cb;
        cb.num_players = 2; cb.local_player = 1; cb.seed = seed;
        a = std::make_unique<Session>(ca,
            std::make_unique<BorrowedTransport>(hub.transport(0)),
            init_arena, step_arena);
        b = std::make_unique<Session>(cb,
            std::make_unique<BorrowedTransport>(hub.transport(1)),
            init_arena, step_arena);
    }
};

}  // namespace

TEST_CASE("zero-latency: two sessions are byte-identical for 200 frames") {
    TwoSession ts;
    for (int t = 0; t < 200; ++t) {
        ts.a->tick(ai_input(static_cast<std::uint32_t>(t), 0));
        ts.b->tick(ai_input(static_cast<std::uint32_t>(t), 1));
        ts.hub.advance_tick();
    }
    int matched = 0, total = 0;
    for (std::uint32_t f = ts.a->current_frame() - 16;
         f <= ts.a->current_frame(); ++f) {
        auto sa = ts.a->snapshot_for(f);
        auto sb = ts.b->snapshot_for(f);
        if (sa.empty() || sb.empty()) continue;
        ++total;
        if (sa.size() == sb.size() &&
            std::equal(sa.begin(), sa.end(), sb.begin())) ++matched;
    }
    REQUIRE(total > 0);
    CHECK(matched == total);
    CHECK_FALSE(ts.a->stats().desync_detected);
    CHECK_FALSE(ts.b->stats().desync_detected);
}

TEST_CASE("150 ms RTT + 5% loss: hashes converge after drain, rollback bounded") {
    NetSimConfig nc;
    nc.latency_ticks = 9;       // ~150 ms RTT at 60 Hz
    nc.jitter_ticks  = 2;
    nc.loss_pct      = 5;
    nc.seed          = 0xABCDEF12345ULL;
    TwoSession ts(nc);
    constexpr int N = 600;
    for (int t = 0; t < N; ++t) {
        ts.a->tick(ai_input(static_cast<std::uint32_t>(t), 0));
        ts.b->tick(ai_input(static_cast<std::uint32_t>(t), 1));
        ts.hub.advance_tick();
    }
    // Drain network with empty inputs so all in-flight packets land.
    for (int t = 0; t < 60; ++t) {
        ts.a->tick(PlayerInput{});
        ts.b->tick(PlayerInput{});
        ts.hub.advance_tick();
    }
    int matched = 0, total = 0;
    for (std::uint32_t f = ts.a->current_frame() - 30;
         f < ts.a->current_frame() - 10; ++f) {
        auto sa = ts.a->snapshot_for(f);
        auto sb = ts.b->snapshot_for(f);
        if (sa.empty() || sb.empty()) continue;
        ++total;
        if (sa.size() == sb.size() &&
            std::equal(sa.begin(), sa.end(), sb.begin())) ++matched;
    }
    REQUIRE(total > 0);
    CHECK(matched == total);
    CHECK_FALSE(ts.a->stats().desync_detected);
    CHECK_FALSE(ts.b->stats().desync_detected);
    double avg_a = static_cast<double>(ts.a->stats().total_rollback_frames) /
                   static_cast<double>(ts.a->stats().total_ticks);
    double avg_b = static_cast<double>(ts.b->stats().total_rollback_frames) /
                   static_cast<double>(ts.b->stats().total_ticks);
    CHECK(avg_a < 6.0);
    CHECK(avg_b < 6.0);
}
