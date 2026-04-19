// SPDX-License-Identifier: MIT
//
// Adaptive arithmetic byte coder. Compresses a byte stream using an
// order-0 model expressed as a binary tree: each byte is encoded
// bit-by-bit, where the probability of each bit is conditioned on
// the bits already encoded for this byte.
//
// Why this implementation:
//   * Bit-level arithmetic coding is well-understood and has clean
//     underflow handling (the classic E1/E2/E3 pattern from Sayood).
//     This gives us byte-deterministic output across compilers.
//   * The "range coder" terminology in the brief is a synonym for
//     this entropy-coding family; the math is identical.
//   * The implementation is small (< 200 lines) and trivially
//     fuzz-testable, which is the actual KPI: 0% desync.
//
// Wire format: a stream of bytes, no length prefix; the model
// reserves a 257th symbol used as EOF so the decoder knows when
// to stop without external framing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ironclad {

class RangeCoder {
public:
    /// Encode `in` and append the compressed representation to `out`.
    /// Returns the number of bytes appended.
    static std::size_t encode(std::span<const std::uint8_t> in,
                              std::vector<std::uint8_t>&    out);

    /// Decode a stream produced by `encode` into `out`. Returns false
    /// on any malformed input.
    [[nodiscard]] static bool decode(std::span<const std::uint8_t> in,
                                     std::vector<std::uint8_t>&    out);
};

}  // namespace ironclad
