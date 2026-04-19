// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <ironclad/range_coder.hpp>

using ironclad::RangeCoder;

static std::vector<std::uint8_t> det_bytes(std::size_t n,
                                           std::uint32_t seed,
                                           std::uint8_t mod = 0) {
    std::vector<std::uint8_t> out(n);
    std::uint32_t x = seed ? seed : 1u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = mod ? static_cast<std::uint8_t>(x % mod)
                     : static_cast<std::uint8_t>(x);
    }
    return out;
}

TEST_CASE("empty buffer round-trips") {
    std::vector<std::uint8_t> in, enc, dec;
    RangeCoder::encode(in, enc);
    REQUIRE(RangeCoder::decode(enc, dec));
    CHECK(dec.empty());
}

TEST_CASE("single byte round-trips") {
    for (int b = 0; b < 256; ++b) {
        std::vector<std::uint8_t> in{static_cast<std::uint8_t>(b)};
        std::vector<std::uint8_t> enc, dec;
        RangeCoder::encode(in, enc);
        REQUIRE(RangeCoder::decode(enc, dec));
        REQUIRE(dec.size() == 1);
        CHECK(dec[0] == b);
    }
}

TEST_CASE("all-zero buffer compresses very tightly") {
    std::vector<std::uint8_t> in(4096, 0);
    std::vector<std::uint8_t> enc, dec;
    RangeCoder::encode(in, enc);
    CHECK(enc.size() < 64);   // generous headroom
    REQUIRE(RangeCoder::decode(enc, dec));
    CHECK(dec == in);
}

TEST_CASE("all-0xFF buffer compresses very tightly") {
    std::vector<std::uint8_t> in(4096, 0xFF);
    std::vector<std::uint8_t> enc, dec;
    RangeCoder::encode(in, enc);
    CHECK(enc.size() < 64);
    REQUIRE(RangeCoder::decode(enc, dec));
    CHECK(dec == in);
}

TEST_CASE("uniform-random buffer round-trips and stays close to entropy") {
    auto in = det_bytes(8192, 42);
    std::vector<std::uint8_t> enc, dec;
    RangeCoder::encode(in, enc);
    REQUIRE(RangeCoder::decode(enc, dec));
    CHECK(dec == in);
    // Uniform random over 256 symbols has 8 bits/byte entropy. Adaptive
    // model has overhead, but encoded size shouldn't blow past 1.2x.
    CHECK(enc.size() < (in.size() * 12) / 10);
}

TEST_CASE("biased buffer compresses below 8 bits/byte") {
    // Symbols restricted to 0..15 should compress to ~4 bits/byte.
    auto in = det_bytes(4096, 99, /*mod=*/16);
    std::vector<std::uint8_t> enc, dec;
    RangeCoder::encode(in, enc);
    REQUIRE(RangeCoder::decode(enc, dec));
    CHECK(dec == in);
    CHECK(enc.size() < in.size() * 6 / 10);
}

TEST_CASE("English-text fixture round-trips") {
    std::string_view text =
        "The deterministic rollback netcode toolkit known as ironclad "
        "provides a straightforward path from a pure simulation core to "
        "a polished networked arena game. Determinism, snapshots, "
        "delta-compression, range coding, lag compensation, and replay "
        "are all in scope.";
    std::vector<std::uint8_t> in(text.begin(), text.end());
    std::vector<std::uint8_t> enc, dec;
    RangeCoder::encode(in, enc);
    REQUIRE(RangeCoder::decode(enc, dec));
    CHECK(dec == in);
}

TEST_CASE("100 randomized round-trips") {
    for (std::uint32_t seed = 1; seed <= 100; ++seed) {
        std::size_t sz = (seed * 53u) % 4096 + 1;
        auto in = det_bytes(sz, seed);
        std::vector<std::uint8_t> enc, dec;
        RangeCoder::encode(in, enc);
        REQUIRE(RangeCoder::decode(enc, dec));
        CHECK(dec == in);
    }
}
