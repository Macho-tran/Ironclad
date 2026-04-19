// SPDX-License-Identifier: MIT
//
// Wire packet schema for ironclad sessions.
//
// Each peer broadcasts an `InputPacket` once per tick (and may also
// piggyback on every send). The packet contains:
//
//   * `header`  — protocol version, sender id, current frame.
//   * `inputs`  — the last `count` inputs the sender produced
//                 (frames `[frame - count + 1 .. frame]`).
//   * `last_known_remote_hash{frame, hash}` — the most recent peer
//     state hash we've verified locally; included so the receiver
//     can detect a desync and react.
//
// Schema version is stored as a single byte at the start of the
// packet so future upgrades can be detected and rejected cleanly.
#pragma once

#include <array>
#include <cstdint>

#include "byteio.hpp"
#include "config.hpp"
#include "input.hpp"

namespace ironclad {

inline constexpr std::uint8_t kPacketVersion = 1;

struct InputPacket {
    std::uint8_t  sender         = 0;
    std::uint32_t frame          = 0;     // most recent frame in `inputs`
    std::uint8_t  count          = 0;     // number of inputs (1..kMaxInputs)
    std::uint32_t ack_frame      = 0;     // most recent peer-state-hash we've verified
    std::uint64_t ack_hash       = 0;     // ...and the value
    static constexpr std::uint8_t kMaxInputs = 16;
    std::array<PlayerInput, kMaxInputs> inputs{};
};

inline void write_input_packet(ByteWriter& w, const InputPacket& p) {
    w.write_u8(kPacketVersion);
    w.write_u8(p.sender);
    w.write_u32(p.frame);
    w.write_u8(p.count);
    w.write_u32(p.ack_frame);
    w.write_u64(p.ack_hash);
    for (std::uint8_t i = 0; i < p.count; ++i) pack(w, p.inputs[i]);
}

[[nodiscard]] inline bool read_input_packet(ByteReader& r, InputPacket& p) {
    const auto ver  = r.read_u8();
    if (ver != kPacketVersion) return false;
    p.sender    = r.read_u8();
    p.frame     = r.read_u32();
    p.count     = r.read_u8();
    p.ack_frame = r.read_u32();
    p.ack_hash  = r.read_u64();
    if (p.count > InputPacket::kMaxInputs) return false;
    for (std::uint8_t i = 0; i < p.count; ++i) {
        p.inputs[i] = unpack_input(r);
    }
    return !r.error();
}

}  // namespace ironclad
