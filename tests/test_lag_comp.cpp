// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <ironclad/lag_comp.hpp>

using namespace ironclad;

// Build a lag buffer where one target moves along +x at speed 1 unit/tick.
// Without lag compensation, an attack at tick 30 aimed at pos at tick 25
// would miss because the target is now 5 units further along. With
// lag compensation rewinding by 5 ticks, the attack lands.
static LagBuffer make_moving_target_buffer() {
    LagBuffer buf(64);
    for (std::uint32_t t = 0; t <= 60; ++t) {
        LagSample s{1, Vec2{Fixed{static_cast<int>(t)}, Fixed{0}},
                    Fixed::from_ratio(1, 2)};
        buf.record(t, {s});
    }
    return buf;
}

TEST_CASE("hit-scan without lag comp aimed at past pos misses") {
    auto buf = make_moving_target_buffer();
    // Attacker at (25, 1.5) aiming straight down (0, -1) at the spot
    // where the target *was* at tick 25. With no rewind, target is at
    // x=30 now, so the ray misses.
    auto hit = buf.hitscan(Vec2{Fixed{25}, Fixed{1}},
                           Vec2{Fixed{0},  Fixed{-1}},
                           Fixed{4}, 30, /*rtt_ticks=*/0);
    CHECK_FALSE(hit.has_value());
}

TEST_CASE("hit-scan with appropriate lag comp rewinds and hits") {
    auto buf = make_moving_target_buffer();
    // Same shot, but RTT = 10 ticks so we rewind 5 ticks. Target at
    // tick 25 was at x=25 -> ray straight down hits.
    auto hit = buf.hitscan(Vec2{Fixed{25}, Fixed{1}},
                           Vec2{Fixed{0},  Fixed{-1}},
                           Fixed{4}, 30, /*rtt_ticks=*/10);
    REQUIRE(hit.has_value());
    CHECK(*hit == 1u);
}

TEST_CASE("hit-scan at target tick lands") {
    auto buf = make_moving_target_buffer();
    auto hit = buf.hitscan(Vec2{Fixed{0}, Fixed{1}},
                           Vec2{Fixed{0}, Fixed{-1}},
                           Fixed{4}, 0, 0);
    REQUIRE(hit.has_value());
    CHECK(*hit == 1u);
}

TEST_CASE("hit-scan misses when target is out of range") {
    auto buf = make_moving_target_buffer();
    auto hit = buf.hitscan(Vec2{Fixed{0}, Fixed{50}},
                           Vec2{Fixed{0}, Fixed{-1}},
                           Fixed{4}, 0, 0);
    CHECK_FALSE(hit.has_value());
}

TEST_CASE("hit-scan returns nullopt when rewind exceeds buffer") {
    auto buf = make_moving_target_buffer();
    // Record stops at 60; query rewinds to 60 - 200/2 = -40 -> below 0.
    auto hit = buf.hitscan(Vec2{Fixed{0}, Fixed{1}},
                           Vec2{Fixed{0}, Fixed{-1}},
                           Fixed{4}, 60, /*rtt_ticks=*/200);
    CHECK_FALSE(hit.has_value());
}

TEST_CASE("multiple targets: closest along ray wins") {
    LagBuffer buf(8);
    LagSample s1{1, Vec2{Fixed{2}, Fixed{0}}, Fixed::from_ratio(1, 2)};
    LagSample s2{2, Vec2{Fixed{5}, Fixed{0}}, Fixed::from_ratio(1, 2)};
    buf.record(0, {s1, s2});
    auto hit = buf.hitscan(Vec2{Fixed{0}, Fixed{0}},
                           Vec2{Fixed{1}, Fixed{0}},
                           Fixed{10}, 0, 0);
    REQUIRE(hit.has_value());
    CHECK(*hit == 1u);
}
