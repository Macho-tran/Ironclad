// SPDX-License-Identifier: MIT
//
// libFuzzer harness for read_input_packet.
//
// Goal: arbitrary bytes -> never crash, never OOB.
#include <cstddef>
#include <cstdint>

#include <ironclad/byteio.hpp>
#include <ironclad/packet.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
    ironclad::ByteReader r(data, size);
    ironclad::InputPacket ip;
    (void)ironclad::read_input_packet(r, ip);
    return 0;
}
