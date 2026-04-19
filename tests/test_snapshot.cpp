// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <ironclad/input.hpp>
#include <ironclad/snapshot.hpp>

using namespace ironclad;

TEST_CASE("PlayerInput layout is bit-exact 4 bytes") {
    static_assert(sizeof(PlayerInput) == 4);
    PlayerInput in{};
    in.buttons = PlayerInput::kAttack | PlayerInput::kDash;
    in.move_x  = -127;
    in.move_y  = 64;
    CHECK(in.attack());
    CHECK(in.dash());
    CHECK(in.move_x_fx() == Fixed{-1});       // -127/127 == -1 exactly
    auto y = in.move_y_fx();                  // 64/127, just check sign + bound
    CHECK(y > Fixed{0});
    CHECK(y < Fixed{1});
}

TEST_CASE("PlayerInput pack/unpack round-trips byte-identically") {
    PlayerInput in;
    in.buttons = 0xABCD;
    in.move_x  = -33;
    in.move_y  = 77;

    ByteWriter w;
    pack(w, in);
    ByteReader r(w.view().data(), w.size());
    auto out = unpack_input(r);
    CHECK(in == out);
    CHECK(w.size() == 4u);
}

TEST_CASE("SnapshotRing stores and retrieves by frame") {
    SnapshotRing ring(8);
    for (std::uint32_t f = 0; f < 8; ++f) {
        ring.store(f, f * 11u, {std::uint8_t(f)});
    }
    for (std::uint32_t f = 0; f < 8; ++f) {
        const auto* s = ring.get(f);
        REQUIRE(s != nullptr);
        CHECK(s->frame == f);
        CHECK(s->hash  == f * 11u);
        REQUIRE(s->bytes.size() == 1);
        CHECK(s->bytes[0] == f);
    }
}

TEST_CASE("SnapshotRing wraps and overwrites the oldest frame") {
    SnapshotRing ring(4);
    for (std::uint32_t f = 0; f < 4; ++f) ring.store(f, 0, {});
    // Now write frames 4..7 — they wrap and overwrite frames 0..3.
    for (std::uint32_t f = 4; f < 8; ++f) ring.store(f, 0, {});
    for (std::uint32_t f = 0; f < 4; ++f) CHECK(ring.get(f) == nullptr);
    for (std::uint32_t f = 4; f < 8; ++f) CHECK(ring.get(f) != nullptr);
    CHECK(ring.oldest_frame() == 4u);
}

TEST_CASE("FrameRing of inputs round-trips and rejects forgotten frames") {
    FrameRing<PlayerInput> ring(8);
    for (std::uint32_t f = 0; f < 8; ++f) {
        PlayerInput in{};
        in.move_x = static_cast<std::int8_t>(f);
        ring.store(f, in);
    }
    for (std::uint32_t f = 0; f < 8; ++f) {
        const auto* in = ring.get(f);
        REQUIRE(in != nullptr);
        CHECK(in->move_x == static_cast<std::int8_t>(f));
    }
    // Overwriting frame 8 evicts frame 0.
    PlayerInput later{};
    later.move_x = 50;
    ring.store(8, later);
    CHECK(ring.get(0) == nullptr);
    REQUIRE(ring.get(8) != nullptr);
    CHECK(ring.get(8)->move_x == 50);
}
