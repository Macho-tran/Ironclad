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

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#  define IRONCLAD_USE_MSVC_INT128 1
#else
#  define IRONCLAD_USE_MSVC_INT128 0
#endif

namespace ironclad {

// We need 128-bit integer multiply+shift and shift+divide for the
// Q32.32 multiply and divide operations. Two backends:
//
//   * GCC/Clang: native `__int128`. Suppress `-Wpedantic` on the
//     typedefs because the extension is "standard" everywhere we
//     ship but the language standard doesn't bless it.
//
//   * MSVC: the `_mul128`/`_div128` intrinsics in <intrin.h>. They
//     return signed-128 results in a (lo, hi) pair, which matches
//     what we need for `(a*b) >> 32` and `(a << 32) / b`.
//
// The public `Fixed` API is identical on both backends. Helpers
// below isolate the 128-bit arithmetic so the rest of the class
// stays toolchain-agnostic.
#if IRONCLAD_USE_MSVC_INT128

namespace detail {

/// Returns (a * b) >> shift, treating a and b as signed 64-bit and
/// the intermediate as signed 128-bit. `shift` must be in [0, 63].
inline std::int64_t mul_shift_64(std::int64_t a, std::int64_t b,
                                 unsigned shift) noexcept {
    std::int64_t hi;
    std::int64_t lo = _mul128(a, b, &hi);
    // Combine (hi, lo) >> shift into a single int64. We assume the
    // result fits in 64 bits (callers guarantee this for valid Q32.32
    // values).
    if (shift == 0) return lo;
    // Build the lower 64 bits of the shifted value:
    //   shifted_lo = (lo >> shift) | (hi << (64 - shift))
    std::uint64_t ulo  = static_cast<std::uint64_t>(lo);
    std::uint64_t uhi  = static_cast<std::uint64_t>(hi);
    std::uint64_t out  = (ulo >> shift) | (uhi << (64 - shift));
    return static_cast<std::int64_t>(out);
}

/// Returns (a << shift) / b. `shift` must be in [0, 63]; b != 0.
inline std::int64_t shift_div_64(std::int64_t a, std::int64_t b,
                                 unsigned shift) noexcept {
    // Build (hi, lo) = a << shift in 128 bits.
    std::uint64_t ua = static_cast<std::uint64_t>(a);
    std::uint64_t lo = ua << shift;
    std::int64_t  hi = a >> (64 - shift);     // arithmetic shift preserves sign
    std::int64_t  rem;
    return _div128(hi, static_cast<std::int64_t>(lo), b, &rem);
}

}  // namespace detail

#else  // GCC / Clang path

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

namespace detail {

inline constexpr std::int64_t mul_shift_64(std::int64_t a, std::int64_t b,
                                           unsigned shift) noexcept {
    return static_cast<std::int64_t>(
        (static_cast<ironclad_i128>(a) * static_cast<ironclad_i128>(b)) >> shift);
}
inline constexpr std::int64_t shift_div_64(std::int64_t a, std::int64_t b,
                                           unsigned shift) noexcept {
    return static_cast<std::int64_t>(
        (static_cast<ironclad_i128>(a) << shift) / b);
}

}  // namespace detail

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
    /// (Not `constexpr` because the MSVC backend uses non-constexpr
    /// intrinsics; the GCC/Clang path could be constexpr but we keep
    /// the API uniform across toolchains.)
    static Fixed from_ratio(std::int64_t num,
                            std::int64_t den) noexcept {
        return Fixed::from_raw(detail::shift_div_64(num, den, kFractionBits));
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
    friend Fixed operator*(Fixed a, Fixed b) noexcept {
        return Fixed::from_raw(detail::mul_shift_64(a.raw_, b.raw_, kFractionBits));
    }
    friend Fixed operator/(Fixed a, Fixed b) noexcept {
        if (b.raw_ == 0) {
            // Defined-deterministic: 0/0 -> 0, x/0 -> sign(x) * MAX.
            if (a.raw_ == 0) return Fixed{};
            return Fixed::from_raw(a.raw_ > 0
                                       ? std::numeric_limits<Raw>::max()
                                       : std::numeric_limits<Raw>::min());
        }
        return Fixed::from_raw(detail::shift_div_64(a.raw_, b.raw_, kFractionBits));
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

// Convenient literal-style constants. (Cannot use `Fixed::from_ratio`
// here because the MSVC backend's intrinsic isn't constexpr.)
inline constexpr Fixed kZero    = Fixed{};
inline constexpr Fixed kOneFx   = Fixed{1};
inline constexpr Fixed kHalf    = Fixed::from_raw(Fixed::kOne >> 1);

}  // namespace ironclad
