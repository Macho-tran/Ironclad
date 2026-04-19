// SPDX-License-Identifier: MIT
#include <ironclad/hash.hpp>

// xxhash.h is a single-header implementation; the actual code is
// instantiated in third_party/xxhash/xxhash.c.
#include "xxhash.h"

namespace ironclad {

std::uint64_t hash64(std::span<const std::byte> bytes,
                     std::uint64_t seed) noexcept {
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

std::uint64_t hash64(const void* data,
                     std::size_t size,
                     std::uint64_t seed) noexcept {
    return XXH3_64bits_withSeed(data, size, seed);
}

}  // namespace ironclad
