// SPDX-License-Identifier: MIT
//
// libFuzzer harness for Delta::decode.
//
// We split the input into a "prev" buffer and a "delta" buffer at
// the byte indicated in the first byte. Then decode. Must not crash
// on any combination.
#include <cstddef>
#include <cstdint>
#include <vector>

#include <ironclad/delta.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
    if (size < 2) return 0;
    std::size_t split = data[0];
    if (split > size - 1) split = size - 1;
    std::vector<std::uint8_t> prev(data + 1, data + 1 + split);
    std::vector<std::uint8_t> delta(data + 1 + split, data + size);
    std::vector<std::uint8_t> out;
    (void)ironclad::Delta::decode({prev.data(), prev.size()},
                                  {delta.data(), delta.size()}, out);
    return 0;
}
