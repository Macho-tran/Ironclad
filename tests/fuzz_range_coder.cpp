// SPDX-License-Identifier: MIT
//
// libFuzzer harness for the adaptive arithmetic range coder.
//
// Two paths: random bytes through encode->decode (must round-trip)
// and arbitrary bytes through decode (must not crash).
#include <cstddef>
#include <cstdint>
#include <vector>

#include <ironclad/range_coder.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
    if (size == 0) return 0;
    // Mode select via first byte.
    const std::uint8_t mode = data[0];
    const std::uint8_t* payload = data + 1;
    const std::size_t   plen    = size - 1;

    if ((mode & 1) == 0) {
        // Round-trip: encode payload, then decode.
        std::vector<std::uint8_t> enc;
        ironclad::RangeCoder::encode({payload, plen}, enc);
        std::vector<std::uint8_t> dec;
        if (!ironclad::RangeCoder::decode({enc.data(), enc.size()}, dec)) {
            // Encoder produced something the decoder can't read —
            // that's a bug; abort to surface it via libFuzzer.
            __builtin_trap();
        }
        // dec must equal original payload.
        if (dec.size() != plen) __builtin_trap();
        for (std::size_t i = 0; i < plen; ++i) {
            if (dec[i] != payload[i]) __builtin_trap();
        }
    } else {
        // Decode arbitrary garbage — must not crash.
        std::vector<std::uint8_t> dec;
        (void)ironclad::RangeCoder::decode({payload, plen}, dec);
    }
    return 0;
}
