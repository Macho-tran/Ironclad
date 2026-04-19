// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <cmath>
#include <initializer_list>
#include <limits>

#include <ironclad/fixed.hpp>

using ironclad::Fixed;

TEST_CASE("integer construction round-trips via to_int") {
    for (int v : {0, 1, -1, 7, -7, 12345, -12345, 1'000'000, -1'000'000}) {
        CHECK(Fixed{v}.to_int() == v);
    }
}

TEST_CASE("equality and ordering") {
    CHECK(Fixed{3} == Fixed{3});
    CHECK(Fixed{3} != Fixed{4});
    CHECK(Fixed{2} <  Fixed{3});
    CHECK(Fixed{3} <= Fixed{3});
    CHECK(Fixed{4} >  Fixed{3});
    CHECK(Fixed{3} >= Fixed{3});
    CHECK(Fixed::from_ratio(1, 2) < Fixed{1});
    CHECK(Fixed::from_ratio(-1, 2) < Fixed{0});
}

TEST_CASE("addition and subtraction are exact for integers") {
    for (int a : {-100, -3, 0, 3, 100}) {
        for (int b : {-100, -3, 0, 3, 100}) {
            CHECK((Fixed{a} + Fixed{b}).to_int() == a + b);
            CHECK((Fixed{a} - Fixed{b}).to_int() == a - b);
        }
    }
}

TEST_CASE("multiplication of integers is exact") {
    for (int a : {-100, -7, -1, 0, 1, 7, 100}) {
        for (int b : {-100, -7, -1, 0, 1, 7, 100}) {
            CHECK((Fixed{a} * Fixed{b}).to_int() == a * b);
        }
    }
}

TEST_CASE("from_ratio gives correct fractional values") {
    CHECK(Fixed::from_ratio(1, 2).raw() == Fixed::kOne / 2);
    CHECK(Fixed::from_ratio(3, 4).raw() == (Fixed::kOne / 4) * 3);
    CHECK(Fixed::from_ratio(-1, 4).raw() == -Fixed::kOne / 4);
    // 1/3 and 2/3 each truncate by 1 ulp, so sum is at most 1 ulp short of 1.
    auto third_sum = (Fixed::from_ratio(1, 3) + Fixed::from_ratio(2, 3)).raw();
    auto err = Fixed::kOne - third_sum;
    if (err < 0) err = -err;
    CHECK(err <= 1);
}

TEST_CASE("multiplication of fractions is within 1 ulp") {
    // (1/2) * (1/2) = 1/4
    Fixed half = Fixed::from_ratio(1, 2);
    Fixed quarter = half * half;
    CHECK(quarter == Fixed::from_ratio(1, 4));
    // (3/2) * (4/3) = 2
    Fixed three_halves = Fixed::from_ratio(3, 2);
    Fixed four_thirds  = Fixed::from_ratio(4, 3);
    Fixed prod         = three_halves * four_thirds;
    auto diff = prod.raw() - Fixed{2}.raw();
    if (diff < 0) diff = -diff;
    CHECK(diff <= 2);  // <=1 ulp from each operand's rounding
}

TEST_CASE("division round-trips multiplication within tolerance") {
    Fixed a = Fixed::from_ratio(355, 113);  // ~pi
    Fixed b = Fixed::from_ratio(7, 5);
    Fixed q = a / b;
    Fixed r = q * b;
    auto diff = r.raw() - a.raw();
    if (diff < 0) diff = -diff;
    CHECK(diff <= 4);
}

TEST_CASE("division by zero is deterministic, not UB") {
    Fixed pos = Fixed{1} / Fixed{0};
    Fixed neg = Fixed{-1} / Fixed{0};
    Fixed zer = Fixed{0} / Fixed{0};
    CHECK(pos.raw() > 0);
    CHECK(neg.raw() < 0);
    CHECK(zer.raw() == 0);
}

TEST_CASE("unary minus saturates rather than overflowing INT64_MIN") {
    Fixed lo = Fixed::from_raw(std::numeric_limits<Fixed::Raw>::min());
    Fixed neg = -lo;
    CHECK(neg.raw() == std::numeric_limits<Fixed::Raw>::max());
}

TEST_CASE("sqrt of perfect squares is exact") {
    for (int n : {0, 1, 4, 9, 16, 25, 100, 144, 10000}) {
        Fixed s = Fixed::sqrt(Fixed{n});
        // Allow 1 ulp.
        auto expected = static_cast<int>(std::sqrt(static_cast<double>(n)));
        auto diff = s.raw() - Fixed{expected}.raw();
        if (diff < 0) diff = -diff;
        CHECK(diff <= 1);
    }
}

TEST_CASE("sqrt of fractional values is within tight bound") {
    // sqrt(2) ~= 1.41421356
    Fixed s = Fixed::sqrt(Fixed{2});
    Fixed s2 = s * s;
    auto diff = s2.raw() - Fixed{2}.raw();
    if (diff < 0) diff = -diff;
    CHECK(diff <= 64);  // <= 64 ulps == 1.5e-8 absolute error
}

TEST_CASE("sqrt of negative is zero (defined behavior)") {
    CHECK(Fixed::sqrt(Fixed{-1}).raw() == 0);
}

TEST_CASE("compound assignment matches binary operator") {
    Fixed a = Fixed::from_ratio(7, 3);
    Fixed b = Fixed::from_ratio(11, 5);
    Fixed s1 = a + b; Fixed s2 = a; s2 += b; CHECK(s1 == s2);
    Fixed d1 = a - b; Fixed d2 = a; d2 -= b; CHECK(d1 == d2);
    Fixed p1 = a * b; Fixed p2 = a; p2 *= b; CHECK(p1 == p2);
    Fixed q1 = a / b; Fixed q2 = a; q2 /= b; CHECK(q1 == q2);
}

TEST_CASE("abs flips sign of negatives, leaves positives") {
    CHECK(Fixed{-3}.abs() == Fixed{3});
    CHECK(Fixed{ 3}.abs() == Fixed{3});
    CHECK(Fixed{ 0}.abs() == Fixed{0});
}
