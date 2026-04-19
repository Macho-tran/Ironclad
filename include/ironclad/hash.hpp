// SPDX-License-Identifier: MIT
//
// Thin wrapper around xxHash3 64-bit. We always hash the *serialized*
// representation of game state, never raw struct memory: that way we
// never accidentally hash padding bytes or pointer values, both of
// which would produce non-deterministic hashes across compilers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ironclad {

/// 64-bit hash of an arbitrary byte span. Stable across architectures
/// and compilers (xxHash3 is a portable algorithm).
[[nodiscard]] std::uint64_t hash64(std::span<const std::byte> bytes,
                                   std::uint64_t seed = 0) noexcept;

/// Convenience overload for raw pointers.
[[nodiscard]] std::uint64_t hash64(const void* data,
                                   std::size_t size,
                                   std::uint64_t seed = 0) noexcept;

}  // namespace ironclad
