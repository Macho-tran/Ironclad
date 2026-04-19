// SPDX-License-Identifier: MIT
//
// Deterministic pseudo-random number generator used by the simulation.
//
// Why we roll our own:
//   * `std::mt19937` is portable in algorithm but its `std::uniform_*`
//     distributions are *not* — different STL vendors yield different
//     bytes for the same engine state. That breaks cross-platform
//     determinism.
//   * `std::random_device` is non-deterministic by construction.
//
// We use SplitMix64 to seed and Xorshift64* to advance, both of which
// produce identical streams given identical state on every supported
// compiler. The state is a single `uint64_t` and is included in every
// snapshot, so rollback restores the RNG along with the world.
#pragma once

#include <cstdint>

namespace ironclad {

class Rng {
public:
    /// Construct from a 64-bit seed. A seed of zero is silently
    /// remapped to a non-zero constant; xorshift state of zero is a
    /// fixed point and would produce all zeros forever.
    constexpr explicit Rng(std::uint64_t seed = 0xC0FFEE'D00D'BEEFULL) noexcept
        : state_(splitmix64(seed == 0 ? 0xDEAD'BEEF'CAFE'BABEULL : seed)) {}

    /// The full internal state. Used by snapshot serialization.
    constexpr std::uint64_t state() const noexcept { return state_; }
    constexpr void          set_state(std::uint64_t s) noexcept { state_ = s; }

    /// Advance and return the next 64-bit value.
    constexpr std::uint64_t next_u64() noexcept {
        // Marsaglia xorshift64* — period 2^64 - 1, passes BigCrush
        // with the suggested multiplier.
        std::uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * 0x2545'F491'4F6C'DD1DULL;
    }

    /// Uniform 32-bit value.
    constexpr std::uint32_t next_u32() noexcept {
        return static_cast<std::uint32_t>(next_u64() >> 32);
    }

    /// Uniform integer in [0, bound). `bound == 0` returns 0. Uses
    /// Lemire's nearly-divisionless rejection method for unbiased
    /// output without per-call division on the fast path.
    constexpr std::uint32_t next_below(std::uint32_t bound) noexcept {
        if (bound == 0) return 0;
        std::uint64_t x = next_u32();
        std::uint64_t m = x * static_cast<std::uint64_t>(bound);
        std::uint32_t lo = static_cast<std::uint32_t>(m);
        if (lo < bound) {
            std::uint32_t t = static_cast<std::uint32_t>(-static_cast<std::int32_t>(bound)) % bound;
            while (lo < t) {
                x = next_u32();
                m = x * static_cast<std::uint64_t>(bound);
                lo = static_cast<std::uint32_t>(m);
            }
        }
        return static_cast<std::uint32_t>(m >> 32);
    }

private:
    /// SplitMix64 step, used only for seeding to spread weak seeds.
    static constexpr std::uint64_t splitmix64(std::uint64_t z) noexcept {
        z = (z + 0x9E37'79B9'7F4A'7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    std::uint64_t state_;
};

}  // namespace ironclad
