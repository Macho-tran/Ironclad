// SPDX-License-Identifier: MIT
#include <ironclad/delta.hpp>

#include <cassert>
#include <cstring>

#include <ironclad/byteio.hpp>

namespace ironclad {

namespace {

// Write `n` (1..2^31) as a little-endian varint.
void write_varint(std::vector<std::uint8_t>& out, std::uint32_t n) {
    while (n >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>((n & 0x7Fu) | 0x80u));
        n >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(n));
}

bool read_varint(const std::uint8_t*& p, const std::uint8_t* end,
                 std::uint32_t&     out) {
    std::uint32_t result = 0;
    unsigned shift  = 0;
    while (true) {
        if (p == end || shift > 28) return false;
        std::uint8_t b = *p++;
        result |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0u) break;
        shift += 7;
    }
    out = result;
    return true;
}

}  // namespace

std::size_t Delta::encode(std::span<const std::uint8_t> prev,
                          std::span<const std::uint8_t> curr,
                          std::vector<std::uint8_t>&    out) {
    assert(prev.size() == curr.size());
    const std::size_t before = out.size();
    const std::size_t n      = prev.size();

    // Header: total uncompressed length, varint.
    write_varint(out, static_cast<std::uint32_t>(n));

    std::size_t i = 0;
    while (i < n) {
        // Count run of zero XOR bytes.
        std::size_t z = i;
        while (z < n && (prev[z] ^ curr[z]) == 0u) ++z;
        if (z > i) {
            std::size_t run = z - i;
            // Tag = 0 -> zero-run, then varint length.
            out.push_back(0u);
            write_varint(out, static_cast<std::uint32_t>(run));
            i = z;
            if (i == n) break;
        }

        // Count run of nonzero XOR bytes.
        std::size_t nz = i;
        while (nz < n && (prev[nz] ^ curr[nz]) != 0u) ++nz;
        std::size_t run = nz - i;
        // Tag = 1 -> literal run, then varint length, then `run` bytes.
        out.push_back(1u);
        write_varint(out, static_cast<std::uint32_t>(run));
        for (std::size_t k = 0; k < run; ++k) {
            out.push_back(static_cast<std::uint8_t>(prev[i + k] ^ curr[i + k]));
        }
        i = nz;
    }

    return out.size() - before;
}

bool Delta::decode(std::span<const std::uint8_t> prev,
                   std::span<const std::uint8_t> delta,
                   std::vector<std::uint8_t>&    out) {
    const std::uint8_t* p   = delta.data();
    const std::uint8_t* end = delta.data() + delta.size();

    std::uint32_t total = 0;
    if (!read_varint(p, end, total)) return false;
    if (total != prev.size())        return false;

    out.resize(total);

    std::size_t i = 0;
    while (i < total) {
        if (p == end) return false;
        std::uint8_t tag = *p++;
        std::uint32_t run = 0;
        if (!read_varint(p, end, run)) return false;
        if (i + run > total) return false;

        if (tag == 0) {
            // Zero-run: copy from prev.
            std::memcpy(out.data() + i, prev.data() + i, run);
        } else if (tag == 1) {
            // Literal-run: XOR each delta byte with prev to recover curr.
            if (static_cast<std::size_t>(end - p) < run) return false;
            for (std::uint32_t k = 0; k < run; ++k) {
                out[i + k] = static_cast<std::uint8_t>(prev[i + k] ^ p[k]);
            }
            p += run;
        } else {
            return false;
        }
        i += run;
    }

    return p == end;
}

}  // namespace ironclad
