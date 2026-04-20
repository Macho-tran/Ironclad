// SPDX-License-Identifier: MIT
//
// Property-based tests. We sample random inputs (deterministically
// seeded) and assert universal properties hold across thousands of
// cases. This is cheap belt-and-braces coverage on top of the
// hand-written cases in the per-subsystem test files.
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <ironclad/byteio.hpp>
#include <ironclad/components.hpp>
#include <ironclad/delta.hpp>
#include <ironclad/ecs.hpp>
#include <ironclad/fixed.hpp>
#include <ironclad/range_coder.hpp>
#include <ironclad/rng.hpp>
#include <ironclad/session.hpp>

using namespace ironclad;

namespace {

std::int64_t next_signed(Rng& rng) {
    return static_cast<std::int64_t>(rng.next_u64());
}

[[maybe_unused]] PlayerInput rand_input(Rng& rng) {
    PlayerInput in;
    in.buttons = static_cast<std::uint16_t>(rng.next_u32());
    in.move_x  = static_cast<std::int8_t>(rng.next_u32());
    in.move_y  = static_cast<std::int8_t>(rng.next_u32());
    return in;
}

}  // namespace

// --- Fixed: properties --------------------------------------------------

TEST_CASE("Fixed: a + b == b + a (commutativity) for many random pairs") {
    Rng rng(0xA110CA7E);
    for (int i = 0; i < 5000; ++i) {
        Fixed a = Fixed::from_raw(next_signed(rng) >> 16);
        Fixed b = Fixed::from_raw(next_signed(rng) >> 16);
        CHECK((a + b) == (b + a));
    }
}

TEST_CASE("Fixed: a == a for any value") {
    Rng rng(0xC0DE);
    for (int i = 0; i < 5000; ++i) {
        Fixed a = Fixed::from_raw(next_signed(rng));
        CHECK(a == a);
        CHECK_FALSE(a != a);
    }
}

TEST_CASE("Fixed: comparison is transitive") {
    Rng rng(0xBA5E);
    for (int i = 0; i < 2000; ++i) {
        Fixed a = Fixed::from_raw(next_signed(rng) >> 4);
        Fixed b = Fixed::from_raw(next_signed(rng) >> 4);
        Fixed c = Fixed::from_raw(next_signed(rng) >> 4);
        if (a < b && b < c) CHECK(a < c);
        if (a == b && b == c) CHECK(a == c);
    }
}

TEST_CASE("Fixed: x - x == 0 (additive inverse)") {
    Rng rng(0xFEED);
    for (int i = 0; i < 5000; ++i) {
        Fixed x = Fixed::from_raw(next_signed(rng) >> 4);
        CHECK((x - x) == kZero);
    }
}

TEST_CASE("Fixed: (x * 2) / 2 == x within 1 ulp on safe range") {
    Rng rng(0xFAFA);
    for (int i = 0; i < 5000; ++i) {
        // Restrict to a range where multiplication by 2 doesn't overflow.
        Fixed x = Fixed::from_raw(next_signed(rng) >> 32);
        Fixed two = Fixed{2};
        Fixed back = (x * two) / two;
        auto diff = back.raw() - x.raw();
        if (diff < 0) diff = -diff;
        CHECK(diff <= 1);
    }
}

TEST_CASE("Fixed: sqrt(x)*sqrt(x) ~= x for non-negative x") {
    Rng rng(0xFEED);
    for (int i = 0; i < 1000; ++i) {
        Fixed x = Fixed::from_raw(static_cast<std::int64_t>(
            rng.next_u64() & 0x0000'FFFF'FFFF'FFFFULL));   // small positive
        Fixed s = Fixed::sqrt(x);
        Fixed back = s * s;
        // Allow up to 0.001 of relative error.
        auto diff = back.raw() - x.raw();
        if (diff < 0) diff = -diff;
        CHECK(diff <= std::max<std::int64_t>(x.raw() / 1000, 64));
    }
}

// --- World: random ops then snapshot byte-equal across freshly-built ----

TEST_CASE("World: random create/destroy/add/remove sequence yields stable bytes") {
    constexpr int kOps = 2000;
    Rng rng(0xC0DEC0DE);
    World a(64), b(64);
    a.register_component<Transform>();
    a.register_component<Velocity>();
    a.register_component<Player>();
    b.register_component<Transform>();
    b.register_component<Velocity>();
    b.register_component<Player>();

    std::vector<Entity> alive_a, alive_b;
    auto apply = [&](World& w, std::vector<Entity>& alive) {
        Rng inner = rng;     // copy so both sides apply same ops
        for (int i = 0; i < kOps; ++i) {
            std::uint32_t op = inner.next_below(4);
            if (op == 0 && alive.size() < 32) {
                Entity e = w.create();
                alive.push_back(e);
            } else if (op == 1 && !alive.empty()) {
                std::uint32_t idx = inner.next_below(static_cast<std::uint32_t>(alive.size()));
                w.destroy(alive[idx]);
                alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(idx));
            } else if (op == 2 && !alive.empty()) {
                std::uint32_t idx = inner.next_below(static_cast<std::uint32_t>(alive.size()));
                w.add<Transform>(alive[idx],
                    Transform{Vec2{Fixed{static_cast<int>(idx)}, Fixed{}}, Fixed{}});
            } else if (op == 3 && !alive.empty()) {
                std::uint32_t idx = inner.next_below(static_cast<std::uint32_t>(alive.size()));
                w.remove<Transform>(alive[idx]);
            }
        }
    };
    apply(a, alive_a);
    rng = Rng(0xC0DEC0DE);     // restart sequence so b sees the same ops
    apply(b, alive_b);

    ByteWriter wa, wb;
    a.serialize(wa); b.serialize(wb);
    REQUIRE(wa.size() == wb.size());
    CHECK(std::equal(wa.view().begin(), wa.view().end(), wb.view().begin()));
}

// --- RangeCoder + Delta: round-trips on many random inputs --------------

TEST_CASE("RangeCoder: 1000 random round-trips") {
    Rng rng(0xC0FFEE);
    for (int i = 0; i < 1000; ++i) {
        std::size_t sz = (rng.next_below(2048) + 1);
        std::vector<std::uint8_t> in(sz);
        for (auto& b : in) b = static_cast<std::uint8_t>(rng.next_u32());
        std::vector<std::uint8_t> enc, dec;
        RangeCoder::encode(in, enc);
        REQUIRE(RangeCoder::decode(enc, dec));
        CHECK(dec == in);
    }
}

TEST_CASE("Delta: 1000 random round-trips of similar buffers") {
    Rng rng(0xBADC0DE);
    for (int i = 0; i < 1000; ++i) {
        std::size_t sz = rng.next_below(1024) + 1;
        std::vector<std::uint8_t> a(sz);
        for (auto& b : a) b = static_cast<std::uint8_t>(rng.next_u32());
        std::vector<std::uint8_t> b = a;
        std::uint32_t mutations = rng.next_below(static_cast<std::uint32_t>(sz / 4 + 1));
        for (std::uint32_t k = 0; k < mutations; ++k) {
            std::size_t idx = rng.next_below(static_cast<std::uint32_t>(sz));
            b[idx] ^= static_cast<std::uint8_t>(rng.next_u32());
        }
        std::vector<std::uint8_t> delta, recovered;
        Delta::encode(a, b, delta);
        REQUIRE(Delta::decode(a, delta, recovered));
        CHECK(recovered == b);
    }
}
