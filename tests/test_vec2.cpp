// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <initializer_list>

#include <ironclad/vec2.hpp>

using ironclad::Fixed;
using ironclad::Vec2;

TEST_CASE("Vec2 default-constructs to origin") {
    Vec2 v{};
    CHECK(v.x == Fixed{0});
    CHECK(v.y == Fixed{0});
}

TEST_CASE("addition and subtraction are component-wise") {
    Vec2 a{Fixed{1}, Fixed{2}};
    Vec2 b{Fixed{3}, Fixed{5}};
    CHECK((a + b) == Vec2{Fixed{4}, Fixed{7}});
    CHECK((b - a) == Vec2{Fixed{2}, Fixed{3}});
}

TEST_CASE("scalar multiplication scales both components") {
    Vec2 v{Fixed{2}, Fixed{-3}};
    CHECK((v * Fixed{4}) == Vec2{Fixed{8}, Fixed{-12}});
    CHECK((Fixed{4} * v) == Vec2{Fixed{8}, Fixed{-12}});
}

TEST_CASE("dot product matches algebra") {
    Vec2 a{Fixed{1}, Fixed{2}};
    Vec2 b{Fixed{3}, Fixed{4}};
    CHECK(a.dot(b) == Fixed{1*3 + 2*4});
}

TEST_CASE("length of (3,4) is 5 within tight tolerance") {
    Vec2 v{Fixed{3}, Fixed{4}};
    CHECK(v.length_sq() == Fixed{25});
    Fixed len = v.length();
    auto diff = len.raw() - Fixed{5}.raw();
    if (diff < 0) diff = -diff;
    CHECK(diff <= 4);
}

TEST_CASE("length of axis-aligned unit vectors equals 1") {
    Vec2 ux{Fixed{1}, Fixed{0}};
    Vec2 uy{Fixed{0}, Fixed{1}};
    CHECK(ux.length() == Fixed{1});
    CHECK(uy.length() == Fixed{1});
}

TEST_CASE("compound operators agree with binary operators") {
    Vec2 a{Fixed{1}, Fixed{2}};
    Vec2 b{Fixed{3}, Fixed{4}};
    Vec2 a2 = a; a2 += b;
    CHECK(a2 == a + b);
    Vec2 a3 = a; a3 -= b;
    CHECK(a3 == a - b);
    Vec2 a4 = a; a4 *= Fixed{3};
    CHECK(a4 == a * Fixed{3});
}
