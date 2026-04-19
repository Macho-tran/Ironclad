// SPDX-License-Identifier: MIT
//
// Demo components used by the arena gameplay layer. Every component
// declares an ADL-found `pack`/`unpack` pair that produces a fixed
// number of bytes regardless of compiler padding. This gives the
// snapshot a byte-deterministic representation.
#pragma once

#include <cstdint>

#include "byteio.hpp"
#include "fixed.hpp"
#include "vec2.hpp"

namespace ironclad {

// ----- Transform -------------------------------------------------------
struct Transform {
    Vec2  pos;
    Fixed angle;  // radians (Q32.32) — unused by current sim, reserved
};

inline void pack(ByteWriter& w, const Transform& t) {
    w.write_i64(t.pos.x.raw());
    w.write_i64(t.pos.y.raw());
    w.write_i64(t.angle.raw());
}
inline void unpack(ByteReader& r, Transform& t) {
    t.pos.x = Fixed::from_raw(r.read_i64());
    t.pos.y = Fixed::from_raw(r.read_i64());
    t.angle = Fixed::from_raw(r.read_i64());
}

// ----- Velocity --------------------------------------------------------
struct Velocity {
    Vec2 v;
};

inline void pack(ByteWriter& w, const Velocity& v) {
    w.write_i64(v.v.x.raw());
    w.write_i64(v.v.y.raw());
}
inline void unpack(ByteReader& r, Velocity& v) {
    v.v.x = Fixed::from_raw(r.read_i64());
    v.v.y = Fixed::from_raw(r.read_i64());
}

// ----- Player ----------------------------------------------------------
struct Player {
    std::uint8_t id      = 0;
    std::uint8_t dash_cd = 0;     // simulation ticks remaining on cooldown
    std::uint8_t hit_cd  = 0;     // ticks until next attack allowed
    std::uint8_t alive   = 1;     // 0 once defeated
    Fixed        hp{100};
    std::uint32_t score  = 0;
};

inline void pack(ByteWriter& w, const Player& p) {
    w.write_u8(p.id);
    w.write_u8(p.dash_cd);
    w.write_u8(p.hit_cd);
    w.write_u8(p.alive);
    w.write_i64(p.hp.raw());
    w.write_u32(p.score);
}
inline void unpack(ByteReader& r, Player& p) {
    p.id      = r.read_u8();
    p.dash_cd = r.read_u8();
    p.hit_cd  = r.read_u8();
    p.alive   = r.read_u8();
    p.hp      = Fixed::from_raw(r.read_i64());
    p.score   = r.read_u32();
}

// ----- Projectile ------------------------------------------------------
struct Projectile {
    std::uint32_t owner = 0;   // entity id of the player who fired
    std::uint16_t ttl   = 0;   // ticks remaining before despawn
    std::uint16_t pad   = 0;   // reserved, must be zero for hash stability
};

inline void pack(ByteWriter& w, const Projectile& p) {
    w.write_u32(p.owner);
    w.write_u16(p.ttl);
    w.write_u16(p.pad);
}
inline void unpack(ByteReader& r, Projectile& p) {
    p.owner = r.read_u32();
    p.ttl   = r.read_u16();
    p.pad   = r.read_u16();
}

// ----- Hitbox ----------------------------------------------------------
struct Hitbox {
    Fixed radius;
};

inline void pack(ByteWriter& w, const Hitbox& h) {
    w.write_i64(h.radius.raw());
}
inline void unpack(ByteReader& r, Hitbox& h) {
    h.radius = Fixed::from_raw(r.read_i64());
}

}  // namespace ironclad
