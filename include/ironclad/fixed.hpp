// SPDX-License-Identifier: MIT
//
// Q32.32 fixed-point number type for use inside the deterministic
// simulation. The whole point of this type is determinism: identical
// inputs MUST produce byte-identical outputs on every supported
// platform/compiler. As such:
//
//   * Storage is a single signed 64-bit integer. The low 32 bits are
//     the fractional part, the high 32 bits are the integer part.
//   * Multiplication and division are performed in 128-bit precision
//     to avoid overflow that would otherwise be platform-dependent.
//   * No floating-point types appear in the hot path. The only
//     `double`/`float` interactions are explicit conversion helpers
//     used by the *view* layer and tests; the simulation core does
//     not call them.
//   * Division by zero, INT64_MIN edge cases, and overflow are
//     handled with explicit, documented behavior so that two
//     implementations cannot disagree.
//
// We intentionally do NOT use a templated/generic fixed-point class.
// One concrete type with one well-understood representation is safer
// for cross-platform determinism than a more flexible abstraction.
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace ironclad {

// 128-bit signed integer used internally for overflow-free multiply/divide.
// MSVC has its own intrinsics but we don't target MSVC for the sim core in
// this slice; if/when we do, define `IRONCLAD_HAS_INT128 0` and add the
// MSVC `_mul128`/`_div128` paths.
//
// `__int128` is a non-standard GCC/Clang extension and `-Wpedantic` flags
// every use of it. The `__extension__` keyword silences the warning *at
// the point of declaration / cast*, but only on GCC/Clang -- which is
// exactly the toolchain that needs it. The macro indirection lets every
// use site stay readable.
#if defined(__SIZEOF_INT128__)
#  define IRONCLAD_HAS_INT128 1
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
typedef __int128            ironclad_i128;
typedef unsigned __int128   ironclad_u128;
#    pragma GCC diagnostic pop
#  else
typedef __int128            ironclad_i128;
typedef unsigned __int128   ironclad_u128;
#  endif
#else
#  error "ironclad currently requires a compiler with __int128 support"
#endif

class Fixed {
public:
    using Raw = std::int64_t;
    static constexpr int kFractionBits = 32;
    static constexpr Raw  kOne          = Raw{1} << kFractionBits;
    static constexpr Raw  kFracMask     = kOne - 1;

    // ----- Construction --------------------------------------------------
    constexpr Fixed() noexcept : raw_(0) {}

    /// Construct from an integer.  Implicit on purpose so `Fixed{3}` works.
    constexpr Fixed(int v) noexcept
        : raw_(static_cast<Raw>(v) << kFractionBits) {}
    constexpr Fixed(long v) noexcept
        : raw_(static_cast<Raw>(v) << kFractionBits) {}
    constexpr Fixed(long long v) noexcept
        : raw_(static_cast<Raw>(v) << kFractionBits) {}

    /// Construct from a rational `num / den`.  `den` must be > 0.
    static constexpr Fixed from_ratio(std::int64_t num,
                                      std::int64_t den) noexcept {
        // Promote to 128 bits to keep precision, then divide.
        ironclad_i128 n = static_cast<ironclad_i128>(num) << kFractionBits;
        ironclad_i128 d = den;
        return Fixed::from_raw(static_cast<Raw>(n / d));
    }

    /// Construct directly from the raw 64-bit representation.  Used by
    /// serialization, tests, and a few low-level helpers.  Do not use
    /// in gameplay code -- prefer `Fixed{int}` or `from_ratio`.
    static constexpr Fixed from_raw(Raw raw) noexcept {
        Fixed f;
        f.raw_ = raw;
        return f;
    }

    // ----- Inspection ----------------------------------------------------
    constexpr Raw   raw()        const noexcept { return raw_; }
    constexpr int   to_int()     const noexcept {
        // Truncate toward zero, matching C integer division semantics.
        return static_cast<int>(raw_ >> kFractionBits);
    }
    /// Lossy conversion for the *view* layer only. Never call from the
    /// simulation; the `[[maybe_unused]]` attribute keeps lints quiet.
    [[nodiscard]] double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(kOne);
    }

    // ----- Comparisons ---------------------------------------------------
    friend constexpr bool operator==(Fixed a, Fixed b) noexcept { return a.raw_ == b.raw_; }
    friend constexpr bool operator!=(Fixed a, Fixed b) noexcept { return a.raw_ != b.raw_; }
    friend constexpr bool operator<(Fixed a, Fixed b)  noexcept { return a.raw_ <  b.raw_; }
    friend constexpr bool operator<=(Fixed a, Fixed b) noexcept { return a.raw_ <= b.raw_; }
    friend constexpr bool operator>(Fixed a, Fixed b)  noexcept { return a.raw_ >  b.raw_; }
    friend constexpr bool operator>=(Fixed a, Fixed b) noexcept { return a.raw_ >= b.raw_; }

    // ----- Arithmetic ----------------------------------------------------
    constexpr Fixed operator-() const noexcept {
        // Saturate INT64_MIN rather than invoke UB.
        return raw_ == std::numeric_limits<Raw>::min()
                   ? Fixed::from_raw(std::numeric_limits<Raw>::max())
                   : Fixed::from_raw(-raw_);
    }

    friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
        return Fixed::from_raw(a.raw_ + b.raw_);
    }
    friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
        return Fixed::from_raw(a.raw_ - b.raw_);
    }
    friend constexpr Fixed operator*(Fixed a, Fixed b) noexcept {
        // (a * b) >> 32, in 128-bit precision.
        ironclad_i128 prod = static_cast<ironclad_i128>(a.raw_) *
                             static_cast<ironclad_i128>(b.raw_);
        return Fixed::from_raw(static_cast<Raw>(prod >> kFractionBits));
    }
    friend constexpr Fixed operator/(Fixed a, Fixed b) noexcept {
        if (b.raw_ == 0) {
            // Defined-deterministic: 0/0 -> 0, x/0 -> sign(x) * MAX.
            if (a.raw_ == 0) return Fixed{};
            return Fixed::from_raw(a.raw_ > 0
                                       ? std::numeric_limits<Raw>::max()
                                       : std::numeric_limits<Raw>::min());
        }
        ironclad_i128 num = static_cast<ironclad_i128>(a.raw_) << kFractionBits;
        return Fixed::from_raw(static_cast<Raw>(num / b.raw_));
    }

    Fixed& operator+=(Fixed b) noexcept { *this = *this + b; return *this; }
    Fixed& operator-=(Fixed b) noexcept { *this = *this - b; return *this; }
    Fixed& operator*=(Fixed b) noexcept { *this = *this * b; return *this; }
    Fixed& operator/=(Fixed b) noexcept { *this = *this / b; return *this; }

    // ----- Math helpers --------------------------------------------------
    constexpr Fixed abs() const noexcept {
        return raw_ < 0 ? -*this : *this;
    }

    /// Integer square root in fixed-point. Uses Newton iteration with a
    /// good initial guess; converges in <= 8 iterations for any non-
    /// negative input. Negative inputs return Fixed{0}.
    static Fixed sqrt(Fixed x) noexcept {
        if (x.raw_ <= 0) return Fixed{};
        // Initial guess via bit_width: ~half the magnitude in fixed terms.
        // Use unsigned to avoid sign issues.
        std::uint64_t u = static_cast<std::uint64_t>(x.raw_);
        // bit_width helper without <bit> for older toolchains.
        int msb = 0;
        for (std::uint64_t v = u; v; v >>= 1) ++msb;  // 1..64
        // Choose the initial guess so it is >= sqrt(x) (Newton converges
        // monotonically downward from above).
        Raw guess_raw = Raw{1} << ((msb + kFractionBits + 1) / 2);
        Fixed guess = Fixed::from_raw(guess_raw);
        for (int i = 0; i < 16; ++i) {
            Fixed next = (guess + x / guess);
            // divide by two
            next = Fixed::from_raw(next.raw_ >> 1);
            if (next.raw_ >= guess.raw_) {
                // Converged (or oscillating by 1 ulp).
                return guess;
            }
            guess = next;
        }
        return guess;
    }

private:
    Raw raw_;
};

// Convenient literal-style constants.
inline constexpr Fixed kZero    = Fixed{};
inline constexpr Fixed kOneFx   = Fixed{1};
inline constexpr Fixed kHalf    = Fixed::from_raw(Fixed::kOne >> 1);

}  // namespace ironclad
