// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <ironclad/hash.hpp>

using ironclad::hash64;

static std::span<const std::byte> as_bytes(std::string_view sv) {
    return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

TEST_CASE("hash64 is deterministic for identical input") {
    std::string_view s = "ironclad rolls back";
    auto a = hash64(as_bytes(s));
    auto b = hash64(as_bytes(s));
    CHECK(a == b);
}

TEST_CASE("different inputs yield different hashes (collision sniff)") {
    auto a = hash64(as_bytes("frame 0"));
    auto b = hash64(as_bytes("frame 1"));
    CHECK(a != b);
}

TEST_CASE("seed parameter changes the hash") {
    auto a = hash64(as_bytes("payload"), 0);
    auto b = hash64(as_bytes("payload"), 1);
    CHECK(a != b);
}

TEST_CASE("known answer for empty input is stable") {
    // We don't pin the exact value here (xxHash3 has its own; pinning
    // would couple us to a vendored version) — but it must be stable
    // across calls in the same binary.
    auto a = hash64(as_bytes(""));
    auto b = hash64(as_bytes(""));
    CHECK(a == b);
}

TEST_CASE("raw-pointer overload matches span overload") {
    std::array<std::uint8_t, 5> buf{1, 2, 3, 4, 5};
    auto a = hash64(buf.data(), buf.size());
    auto b = hash64({reinterpret_cast<const std::byte*>(buf.data()),
                     buf.size()});
    CHECK(a == b);
}

TEST_CASE("hashing a 64 KB random buffer is consistent") {
    std::vector<std::uint8_t> buf(64 * 1024);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::uint8_t>((i * 1103515245u + 12345u) >> 16);
    }
    auto a = hash64(buf.data(), buf.size());
    auto b = hash64(buf.data(), buf.size());
    CHECK(a == b);
    // Mutating one byte changes the hash.
    buf[42] ^= 1;
    auto c = hash64(buf.data(), buf.size());
    CHECK(a != c);
}
