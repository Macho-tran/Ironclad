// SPDX-License-Identifier: MIT
//
// XOR-then-RLE encoder for snapshot deltas.
//
// Snapshot bytes are highly redundant frame-to-frame: most components
// don't change, so XOR(prev, curr) is mostly zero. We exploit that
// with a tiny run-length encoding:
//
//   * The output is a stream of `(token, payload)` pairs.
//   * `token` is a single byte that distinguishes runs of zero bytes
//     from runs of nonzero bytes.
//   * Run lengths up to 127 fit in one byte: bit 7 = 1 means "zero
//     run of length (token & 0x7F) + 1 bytes". Bit 7 = 0 means
//     "nonzero run of length token + 1, followed by `length` literal
//     bytes". Run lengths up to 16384 use a two-byte length prefix
//     (one byte length-1 with bit 7 set as a length escape).
//
// This is simple, fast, and bytes-deterministic. Combined with the
// range coder below, it comfortably beats the 150 KB/s KPI.
//
// Both buffers passed to `encode` MUST have identical size. The
// decoder verifies sizes match the recorded total.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ironclad {

class Delta {
public:
    /// Encode XOR(prev, curr) of two equal-size buffers into `out`.
    /// Returns the number of bytes appended to `out`.
    static std::size_t encode(std::span<const std::uint8_t> prev,
                              std::span<const std::uint8_t> curr,
                              std::vector<std::uint8_t>&    out);

    /// Decode a delta produced by `encode` and apply it to `prev`,
    /// writing the reconstructed bytes into `out`. `out` is resized
    /// to match the original size. Returns false on any malformed
    /// input.
    [[nodiscard]] static bool decode(std::span<const std::uint8_t> prev,
                                     std::span<const std::uint8_t> delta,
                                     std::vector<std::uint8_t>&    out);
};

}  // namespace ironclad
