// SPDX-License-Identifier: MIT
//
// Per-player input. Bit-packed to 4 bytes so packets stay tiny:
// at 60 Hz with N=8 buffered frames per packet that's 32 bytes per
// player per packet, well under any reasonable bandwidth budget.
//
// Determinism notes:
//   * Layout is `static_assert`-pinned to 4 bytes with no padding.
//   * Move axes are 8-bit signed integers. The simulation converts
//     them to Q32.32 with a single fixed-point divide; we never
//     touch float here.
#pragma once

#include <cstdint>
#include <type_traits>

#include "byteio.hpp"
#include "fixed.hpp"

namespace ironclad {

struct PlayerInput {
    /// Bit flags. Bit 0 = attack, bit 1 = dash. Higher bits reserved.
    std::uint16_t buttons = 0;
    /// Left-stick X axis, range [-127, 127], scaled to fixed in sim.
    std::int8_t   move_x  = 0;
    /// Left-stick Y axis, range [-127, 127], scaled to fixed in sim.
    std::int8_t   move_y  = 0;

    static constexpr std::uint16_t kAttack = 1u << 0;
    static constexpr std::uint16_t kDash   = 1u << 1;

    [[nodiscard]] bool attack() const noexcept { return buttons & kAttack; }
    [[nodiscard]] bool dash()   const noexcept { return buttons & kDash; }

    /// Move axis as Q32.32 in the range [-1, 1].
    [[nodiscard]] Fixed move_x_fx() const noexcept {
        return Fixed::from_ratio(move_x, 127);
    }
    [[nodiscard]] Fixed move_y_fx() const noexcept {
        return Fixed::from_ratio(move_y, 127);
    }

    friend constexpr bool operator==(PlayerInput a, PlayerInput b) noexcept {
        return a.buttons == b.buttons &&
               a.move_x  == b.move_x  &&
               a.move_y  == b.move_y;
    }
    friend constexpr bool operator!=(PlayerInput a, PlayerInput b) noexcept {
        return !(a == b);
    }
};

static_assert(sizeof(PlayerInput) == 4,
              "PlayerInput must stay tiny for bandwidth reasons");
static_assert(std::is_standard_layout_v<PlayerInput>,
              "PlayerInput must be standard-layout for bit-exact packets");
static_assert(std::is_trivially_copyable_v<PlayerInput>,
              "PlayerInput must be trivially copyable");

inline void pack(ByteWriter& w, PlayerInput in) {
    w.write_u16(in.buttons);
    w.write_u8(static_cast<std::uint8_t>(in.move_x));
    w.write_u8(static_cast<std::uint8_t>(in.move_y));
}
inline PlayerInput unpack_input(ByteReader& r) {
    PlayerInput in;
    in.buttons = r.read_u16();
    in.move_x  = static_cast<std::int8_t>(r.read_u8());
    in.move_y  = static_cast<std::int8_t>(r.read_u8());
    return in;
}

}  // namespace ironclad
