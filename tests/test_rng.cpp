// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <array>
#include <cstdint>
#include <unordered_set>

#include <ironclad/rng.hpp>

using ironclad::Rng;

// First 8 outputs of `Rng{0xC0FFEE'D00D'BEEFULL}.next_u64()`.
// These were captured once and frozen here: any code change that
// alters the byte-level sequence will fail this test, alerting us
// to a determinism regression that would silently desync replays.
static constexpr std::array<std::uint64_t, 8> kExpectedDefault = {
    0x392965b1523ed182ULL,
    0xcccff05236ae9fc1ULL,
    0x5b16d412871b7820ULL,
    0xf211c943fa449889ULL,
    0x3f87e91133d75e59ULL,
    0xb678f0afd19b85b8ULL,
    0xf68427133ab21189ULL,
    0xaaf874be15132521ULL,
};

TEST_CASE("RNG default seed produces frozen byte sequence") {
    // The constants above are the *current* observed sequence on
    // x86_64 GCC/Clang. If they change, either:
    //   (a) someone tweaked the seed default / xorshift constants,
    //       in which case regenerate this fixture in lock-step; or
    //   (b) we have a determinism regression -- investigate.
    Rng rng;
    for (std::size_t i = 0; i < kExpectedDefault.size(); ++i) {
        CHECK(rng.next_u64() == kExpectedDefault[i]);
    }
}

TEST_CASE("Same seed reproduces same sequence") {
    Rng a{0x12345678ABCD1234ULL};
    Rng b{0x12345678ABCD1234ULL};
    for (int i = 0; i < 1024; ++i) {
        CHECK(a.next_u64() == b.next_u64());
    }
}

TEST_CASE("Different seeds produce different sequences") {
    Rng a{0x1ULL};
    Rng b{0x2ULL};
    int diffs = 0;
    for (int i = 0; i < 32; ++i) {
        if (a.next_u64() != b.next_u64()) ++diffs;
    }
    CHECK(diffs > 16);  // overwhelmingly likely to be all 32
}

TEST_CASE("Zero seed is remapped to non-degenerate state") {
    Rng a{0};
    Rng b{0};
    CHECK(a.next_u64() == b.next_u64());
    CHECK(a.state() != 0u);
}

TEST_CASE("State save/restore reproduces stream") {
    Rng a{0xDEADC0DEFEEDFACEULL};
    for (int i = 0; i < 17; ++i) (void)a.next_u64();
    auto saved = a.state();
    auto next  = a.next_u64();

    Rng b{1};
    b.set_state(saved);
    CHECK(b.next_u64() == next);
}

TEST_CASE("next_below stays within bounds and exercises edge values") {
    Rng rng{42};
    for (std::uint32_t bound : {1u, 2u, 3u, 7u, 31u, 256u, 1000u, 65537u}) {
        for (int i = 0; i < 1024; ++i) {
            auto v = rng.next_below(bound);
            CHECK(v < bound);
        }
    }
    CHECK(rng.next_below(0) == 0u);
}

TEST_CASE("next_below distribution is roughly uniform") {
    // Crude smoke test: 8 buckets x 8000 samples; each bucket should
    // see between 700 and 1300 samples (huge tolerance, just guards
    // against an obvious bias bug).
    Rng rng{123};
    std::array<int, 8> counts{};
    for (int i = 0; i < 8000; ++i) ++counts[rng.next_below(8)];
    for (int c : counts) {
        CHECK(c > 700);
        CHECK(c < 1300);
    }
}
