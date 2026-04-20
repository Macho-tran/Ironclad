// SPDX-License-Identifier: MIT
//
// Stress tests — opt-in via `ctest -L stress`. These take longer
// than the default unit/soak suite and are designed to surface
// long-tail bugs (slow leaks, integer drift, ring-buffer wrap,
// extreme network conditions, max player count).
//
// Each test is capped at a few minutes so even a 1-hour soak fits
// inside CI's free tier when explicitly requested.
#include "doctest.h"

#include <cstdio>

#include "../apps/arena_demo/arena.hpp"

TEST_CASE("stress: extreme netsim — 30s @ 4p @ 50%loss/200msRTT/50%reorder") {
    arena_demo::Options o;
    o.frames      = 1800;          // 30 s @ 60 Hz
    o.num_players = 4;
    o.rtt_ms      = 200;
    o.jitter_ms   = 100;
    o.loss_pct    = 50;
    o.reorder_pct = 50;
    o.seed        = 0xDEAD'D00DULL;
    o.quiet       = true;
    auto r = arena_demo::run(o);
    std::printf("stress-extreme: ticks=%lu rb=%.2ff bw=%.1fKB/s desync=%s\n",
                static_cast<unsigned long>(r.total_ticks),
                r.avg_rollback_frames,
                r.bandwidth_kbps_per_client,
                r.desync_detected ? "YES" : "no");
    CHECK_FALSE(r.desync_detected);
}

TEST_CASE("stress: 8 players — kMaxPlayers session runs cleanly") {
    arena_demo::Options o;
    o.frames      = 600;           // 10 s
    o.num_players = 8;
    o.rtt_ms      = 80;
    o.jitter_ms   = 10;
    o.loss_pct    = 2;
    o.reorder_pct = 1;
    o.seed        = 0x8888'8888ULL;
    o.quiet       = true;
    auto r = arena_demo::run(o);
    std::printf("stress-8p: ticks=%lu rb=%.2ff bw=%.1fKB/s desync=%s\n",
                static_cast<unsigned long>(r.total_ticks),
                r.avg_rollback_frames,
                r.bandwidth_kbps_per_client,
                r.desync_detected ? "YES" : "no");
    CHECK_FALSE(r.desync_detected);
    CHECK(r.total_ticks == 600u);
}

TEST_CASE("stress: extended soak — 5 minutes @ 4p @ 150ms/5%/30ms/2%") {
    arena_demo::Options o;
    o.frames      = 18000;         // 5 minutes @ 60 Hz
    o.num_players = 4;
    o.rtt_ms      = 150;
    o.jitter_ms   = 30;
    o.loss_pct    = 5;
    o.reorder_pct = 2;
    o.seed        = 0xBEEFFEEDC0FFEEULL;
    o.quiet       = true;
    auto r = arena_demo::run(o);
    std::printf("stress-5min: ticks=%lu rb_avg=%.2ff bw=%.1fKB/s desync=%s\n",
                static_cast<unsigned long>(r.total_ticks),
                r.avg_rollback_frames,
                r.bandwidth_kbps_per_client,
                r.desync_detected ? "YES" : "no");
    CHECK_FALSE(r.desync_detected);
    CHECK(r.total_ticks == 18000u);
}
