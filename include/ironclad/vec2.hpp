// SPDX-License-Identifier: MIT
//
// 2D vector of `Fixed` values. Used for entity positions, velocities,
// and the few geometry helpers the demo needs (distance checks for
// projectile / hit-scan tests). Like `Fixed`, it is a POD with no
// allocations, no virtual functions, and no float types.
#pragma once

#include "fixed.hpp"

namespace ironclad {

struct Vec2 {
    Fixed x;
    Fixed y;

    constexpr Vec2() noexcept : x{}, y{} {}
    constexpr Vec2(Fixed xv, Fixed yv) noexcept : x(xv), y(yv) {}

    friend constexpr bool operator==(Vec2 a, Vec2 b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
    friend constexpr bool operator!=(Vec2 a, Vec2 b) noexcept {
        return !(a == b);
    }

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
        return Vec2{a.x + b.x, a.y + b.y};
    }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
        return Vec2{a.x - b.x, a.y - b.y};
    }
    friend constexpr Vec2 operator*(Vec2 a, Fixed s) noexcept {
        return Vec2{a.x * s, a.y * s};
    }
    friend constexpr Vec2 operator*(Fixed s, Vec2 a) noexcept {
        return Vec2{a.x * s, a.y * s};
    }

    Vec2& operator+=(Vec2 b) noexcept { *this = *this + b; return *this; }
    Vec2& operator-=(Vec2 b) noexcept { *this = *this - b; return *this; }
    Vec2& operator*=(Fixed s) noexcept { *this = *this * s; return *this; }

    constexpr Fixed dot(Vec2 b) const noexcept {
        return x * b.x + y * b.y;
    }
    constexpr Fixed length_sq() const noexcept {
        return dot(*this);
    }
    Fixed length() const noexcept {
        return Fixed::sqrt(length_sq());
    }
};

}  // namespace ironclad
