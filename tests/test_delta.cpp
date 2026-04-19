// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <cstdint>
#include <random>
#include <vector>

#include <ironclad/delta.hpp>

using ironclad::Delta;

static std::vector<std::uint8_t> rand_bytes(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> out(n);
    // Tiny xorshift32 — std::mt19937 would do, but this stays
    // hermetic and trivially deterministic.
    std::uint32_t x = seed ? seed : 1u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = static_cast<std::uint8_t>(x);
    }
    return out;
}

TEST_CASE("delta of two identical buffers is tiny and round-trips") {
    auto a = rand_bytes(4096, 1);
    auto b = a;
    std::vector<std::uint8_t> enc;
    auto sz = Delta::encode(a, b, enc);
    CHECK(sz <= 16);   // varint header + a single zero-run

    std::vector<std::uint8_t> dec;
    REQUIRE(Delta::decode(a, enc, dec));
    CHECK(dec == b);
}

TEST_CASE("delta with sparse changes round-trips and is small") {
    auto a = rand_bytes(8192, 2);
    auto b = a;
    // Change one byte every 256 — lots of zero runs.
    for (std::size_t i = 64; i < b.size(); i += 256) b[i] ^= 0xFFu;

    std::vector<std::uint8_t> enc;
    auto sz = Delta::encode(a, b, enc);
    CHECK(sz < a.size() / 4);   // dramatic reduction

    std::vector<std::uint8_t> dec;
    REQUIRE(Delta::decode(a, enc, dec));
    CHECK(dec == b);
}

TEST_CASE("delta of fully random independent buffers still round-trips") {
    auto a = rand_bytes(2048, 3);
    auto b = rand_bytes(2048, 4);
    std::vector<std::uint8_t> enc;
    Delta::encode(a, b, enc);
    std::vector<std::uint8_t> dec;
    REQUIRE(Delta::decode(a, enc, dec));
    CHECK(dec == b);
}

TEST_CASE("decode rejects truncated input") {
    auto a = rand_bytes(64, 5);
    auto b = rand_bytes(64, 6);
    std::vector<std::uint8_t> enc;
    Delta::encode(a, b, enc);
    enc.resize(enc.size() / 2);    // chop in half
    std::vector<std::uint8_t> dec;
    CHECK_FALSE(Delta::decode(a, enc, dec));
}

TEST_CASE("decode rejects size mismatch") {
    auto a = rand_bytes(32, 7);
    auto b = rand_bytes(32, 8);
    std::vector<std::uint8_t> enc;
    Delta::encode(a, b, enc);
    std::vector<std::uint8_t> wrong_prev(64, 0);
    std::vector<std::uint8_t> dec;
    CHECK_FALSE(Delta::decode(wrong_prev, enc, dec));
}

TEST_CASE("encoding empty buffers is well-defined") {
    std::vector<std::uint8_t> a, b, enc;
    Delta::encode(a, b, enc);
    std::vector<std::uint8_t> dec;
    REQUIRE(Delta::decode(a, enc, dec));
    CHECK(dec.empty());
}

TEST_CASE("100 randomized round-trips at varying sizes") {
    for (std::uint32_t seed = 100; seed < 200; ++seed) {
        std::size_t sz = (seed * 37u) % 2048 + 1;
        auto a = rand_bytes(sz, seed);
        auto b = a;
        // Mutate ~10% of bytes with a deterministic xor.
        std::uint32_t x = seed * 17u + 1u;
        for (std::size_t k = 0; k < sz / 10; ++k) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            b[x % sz] ^= static_cast<std::uint8_t>(x >> 8);
        }
        std::vector<std::uint8_t> enc;
        Delta::encode(a, b, enc);
        std::vector<std::uint8_t> dec;
        REQUIRE(Delta::decode(a, enc, dec));
        CHECK(dec == b);
    }
}
