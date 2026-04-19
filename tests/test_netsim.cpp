// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <array>
#include <vector>

#include <ironclad/loopback_transport.hpp>

using namespace ironclad;

static std::vector<std::uint8_t> bytes_for(std::uint64_t v) {
    std::vector<std::uint8_t> out(8);
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(v >> (8u * i));
    }
    return out;
}

static std::uint64_t u64_from(const std::vector<std::uint8_t>& b) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) {
        v |= std::uint64_t{b[i]} << (8u * i);
    }
    return v;
}

TEST_CASE("zero-latency loopback delivers packets the same tick") {
    LoopbackHub hub(2);
    auto* a = hub.transport(0);
    auto* b = hub.transport(1);
    auto p = bytes_for(0xAB'CD'EF);
    a->send(1, p);
    auto rp = b->recv();
    REQUIRE(rp.has_value());
    CHECK(rp->from_player == 0);
    CHECK(rp->bytes == p);
    CHECK_FALSE(b->recv().has_value());
}

TEST_CASE("non-zero latency holds packets until tick advances") {
    NetSimConfig cfg;
    cfg.latency_ticks = 3;
    LoopbackHub hub(2, cfg);
    auto* a = hub.transport(0);
    auto* b = hub.transport(1);
    a->send(1, bytes_for(7));
    CHECK_FALSE(b->recv().has_value());
    hub.advance_tick();           // tick 1
    CHECK_FALSE(b->recv().has_value());
    hub.advance_tick();           // tick 2
    CHECK_FALSE(b->recv().has_value());
    hub.advance_tick();           // tick 3 — deliverable
    CHECK(b->recv().has_value());
}

TEST_CASE("loss percentage drops the right ballpark of packets") {
    NetSimConfig cfg;
    cfg.loss_pct = 20;
    cfg.seed     = 0xABCDEFABCDEFULL;
    LoopbackHub hub(2, cfg);
    auto* a = hub.transport(0);
    auto* b = hub.transport(1);

    constexpr int N = 10000;
    for (int i = 0; i < N; ++i) {
        a->send(1, bytes_for(static_cast<std::uint64_t>(i)));
    }
    int delivered = 0;
    while (b->recv()) ++delivered;
    CHECK(delivered > N * 75 / 100);
    CHECK(delivered < N * 85 / 100);
    CHECK(hub.stats().packets_dropped > 0u);
}

TEST_CASE("netsim is deterministic given the same seed") {
    NetSimConfig cfg;
    cfg.latency_ticks = 5;
    cfg.jitter_ticks  = 5;
    cfg.loss_pct      = 10;
    cfg.reorder_pct   = 20;
    cfg.seed          = 0x1234'5678'9ABCDEF0ULL;

    auto run = [&]() {
        LoopbackHub hub(2, cfg);
        auto* a = hub.transport(0);
        auto* b = hub.transport(1);
        std::vector<std::uint64_t> received;
        for (int t = 0; t < 200; ++t) {
            a->send(1, bytes_for(static_cast<std::uint64_t>(t)));
            while (auto pkt = b->recv()) received.push_back(u64_from(pkt->bytes));
            hub.advance_tick();
        }
        // Drain remaining packets after sending stops.
        for (int t = 0; t < 50; ++t) {
            while (auto pkt = b->recv()) received.push_back(u64_from(pkt->bytes));
            hub.advance_tick();
        }
        return received;
    };

    auto x = run();
    auto y = run();
    CHECK(x == y);
    // We should see some reorders given the params (very likely
    // statistically, fully deterministic given the seed).
    LoopbackHub h(2, cfg);
    auto* a = h.transport(0);
    for (int i = 0; i < 100; ++i) a->send(1, bytes_for(static_cast<std::uint64_t>(i)));
    CHECK(h.stats().packets_reordered > 0u);
}

TEST_CASE("stats track sent/delivered byte counts") {
    LoopbackHub hub(2);
    auto* a = hub.transport(0);
    auto* b = hub.transport(1);
    a->send(1, bytes_for(1));
    a->send(1, bytes_for(2));
    while (b->recv()) {}
    CHECK(hub.stats().packets_sent      == 2u);
    CHECK(hub.stats().packets_delivered == 2u);
    CHECK(hub.stats().bytes_sent        == 16u);
    CHECK(hub.stats().bytes_delivered   == 16u);
}
