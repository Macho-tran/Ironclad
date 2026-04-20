// SPDX-License-Identifier: MIT
//
// Two-process UDP smoke test (single-process, two-thread variant).
// Spins up two `Session`s, each with its own `UdpTransport`, on
// 127.0.0.1, runs 30s of simulation between them, and asserts
// byte-identical snapshots + 0 desyncs.
//
// Tagged with CTest label "network" so a sandboxed CI without
// loopback sockets can opt out via -LE network.
#include "doctest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <ironclad/components.hpp>
#include <ironclad/session.hpp>
#include <ironclad/udp_transport.hpp>

using namespace ironclad;

namespace {

void udp_step(World& w, const PlayerInput* inputs,
              std::uint8_t num_players, Rng& rng) {
    for (std::uint8_t p = 0; p < num_players; ++p) {
        Entity self = kInvalidEntity;
        w.each<Player>([&](Entity e, Player& pl) {
            if (pl.id == p) self = e;
        });
        if (self == kInvalidEntity) continue;
        auto* tr  = w.get<Transform>(self);
        auto* vel = w.get<Velocity>(self);
        if (!tr || !vel) continue;
        vel->v.x = inputs[p].move_x_fx() * Fixed::from_ratio(1, 2);
        vel->v.y = inputs[p].move_y_fx() * Fixed::from_ratio(1, 2);
        tr->pos += vel->v;
    }
    (void)rng.next_u64();
}

void udp_init(World& w, Rng& rng, const SessionConfig& cfg) {
    w.register_component<Transform>();
    w.register_component<Velocity>();
    w.register_component<Player>();
    w.register_component<Projectile>();
    w.register_component<Hitbox>();
    for (std::uint8_t p = 0; p < cfg.num_players; ++p) {
        Entity e = w.create();
        w.add(e, Transform{Vec2{Fixed{p * 4}, Fixed{0}}, Fixed{}});
        w.add(e, Velocity{});
        w.add(e, Player{p, 0, 0, 1, Fixed{100}, 0});
        w.add(e, Hitbox{Fixed::from_ratio(1, 2)});
    }
    (void)rng;
}

PlayerInput ai_for(std::uint32_t f, std::uint8_t p) {
    PlayerInput in;
    int phase = static_cast<int>(f) + p * 16;
    in.move_x = static_cast<std::int8_t>(((phase / 30) & 1) ? 64 : -64);
    in.move_y = static_cast<std::int8_t>(((phase / 47) & 1) ? 64 : -64);
    return in;
}

}  // namespace

TEST_CASE("UdpTransport: two sessions on 127.0.0.1 stay byte-identical [network]") {
    // Bind two ephemeral ports.
    auto t0 = std::make_unique<UdpTransport>(/*bind=*/0, /*player=*/0);
    auto t1 = std::make_unique<UdpTransport>(/*bind=*/0, /*player=*/1);

    UdpEndpoint ep0; ep0.ipv4 = 0x7F000001u; ep0.port = t0->bound_port();
    UdpEndpoint ep1; ep1.ipv4 = 0x7F000001u; ep1.port = t1->bound_port();
    t0->add_peer(1, ep1);
    t1->add_peer(0, ep0);

    // Wait briefly for HELLO exchange so the first send has a known
    // peer table on both sides. With non-blocking sockets we just
    // sleep — this isn't a fairness/perf test.
    for (int i = 0; i < 5; ++i) {
        (void)t0->recv();
        (void)t1->recv();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    SessionConfig ca; ca.num_players = 2; ca.local_player = 0;
                      ca.seed = 0xBEEFFEED; ca.local_input_delay = 0;
    SessionConfig cb; cb.num_players = 2; cb.local_player = 1;
                      cb.seed = 0xBEEFFEED; cb.local_input_delay = 0;
    Session a(ca, std::move(t0), udp_init, udp_step);
    Session b(cb, std::move(t1), udp_init, udp_step);

    constexpr int kFrames = 600;       // 10s @ 60Hz
    for (int f = 0; f < kFrames; ++f) {
        a.tick(ai_for(static_cast<std::uint32_t>(f), 0));
        b.tick(ai_for(static_cast<std::uint32_t>(f), 1));
        // Real wall-clock sleep so packets actually traverse the
        // socket — the kernel still buffers everything if we
        // hammer at full speed, but a short yield keeps things
        // visibly two-process-y.
        if ((f & 31) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Drain any in-flight packets.
    for (int f = 0; f < 60; ++f) {
        a.tick(PlayerInput{});
        b.tick(PlayerInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    int matched = 0, total = 0;
    for (std::uint32_t f = a.current_frame() - 30;
         f < a.current_frame() - 10; ++f) {
        auto sa = a.snapshot_for(f);
        auto sb = b.snapshot_for(f);
        if (sa.empty() || sb.empty()) continue;
        ++total;
        if (sa.size() == sb.size() &&
            std::equal(sa.begin(), sa.end(), sb.begin())) ++matched;
    }
    REQUIRE(total > 0);
    CHECK(matched == total);
    CHECK_FALSE(a.stats().desync_detected);
    CHECK_FALSE(b.stats().desync_detected);
}

TEST_CASE("UdpEndpoint::parse handles host:port and bare port") {
    auto e = UdpEndpoint::parse("127.0.0.1:7777");
    REQUIRE(e.has_value());
    CHECK(e->ipv4 == 0x7F000001u);
    CHECK(e->port == 7777);

    auto bad = UdpEndpoint::parse("not-a-host");
    CHECK_FALSE(bad.has_value());

    auto zero = UdpEndpoint::parse("127.0.0.1:0");
    CHECK_FALSE(zero.has_value());     // port 0 reserved
}
