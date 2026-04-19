// SPDX-License-Identifier: MIT
//
// Replay recorder & player.
//
// File format (.iclr) — all little-endian:
//
//   magic        : "IRCL_REPLAY1" (12 bytes)
//   version      : u16
//   tick_hz      : u16
//   num_players  : u8
//   reserved     : u8 (must be 0)
//   seed         : u64
//   world_cap    : u32
//   init_size    : u32        // length of initial snapshot bytes
//   init_bytes   : u8[init_size]
//   <records>...
//   trailer_magic: "ENDR" (4 bytes)
//   frame_count  : u32
//   final_hash   : u64
//
// Each record:
//   frame        : u32
//   per-player PlayerInput * num_players
//   hash         : u64        // hash of state AFTER this tick
//
// The recorder is intended to be wrapped *around* a Session: every
// `tick`, the host calls `recorder.record(...)` after the session
// step. The replayer is a free function that re-runs a Session from
// the recorded inputs and asserts every recorded hash matches.
#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "input.hpp"

namespace ironclad {

class Recorder {
public:
    /// Begin a recording. `initial_snapshot` is the world state
    /// bytes immediately after `init_arena`.
    void begin(std::uint16_t tick_hz,
               std::uint8_t  num_players,
               std::uint64_t seed,
               std::uint32_t world_capacity,
               std::span<const std::uint8_t> initial_snapshot);

    void record(std::uint32_t frame,
                std::span<const PlayerInput> per_player_inputs,
                std::uint64_t hash);

    /// Finalize and return the complete recording bytes.
    std::vector<std::uint8_t> finish(std::uint64_t final_hash);

    [[nodiscard]] std::uint32_t frame_count() const noexcept { return frames_; }
    [[nodiscard]] std::size_t   bytes_written() const noexcept { return out_.size(); }

private:
    std::vector<std::uint8_t> out_;
    std::uint8_t              num_players_ = 0;
    std::uint32_t             frames_      = 0;
};

/// Header info parsed from a recording file.
struct ReplayHeader {
    std::uint16_t              version       = 0;
    std::uint16_t              tick_hz       = 0;
    std::uint8_t               num_players   = 0;
    std::uint64_t              seed          = 0;
    std::uint32_t              world_capacity = 0;
    std::vector<std::uint8_t>  initial_snapshot;
};

struct ReplayRecord {
    std::uint32_t              frame   = 0;
    std::vector<PlayerInput>   inputs;
    std::uint64_t              hash    = 0;
};

/// Parses a recording. Returns true on success. The trailer's
/// `frame_count` is checked, but the per-record hashes are not
/// re-validated against any simulation here — that's the
/// caller's job (see `replay_and_verify`).
[[nodiscard]] bool parse_replay(std::span<const std::uint8_t> bytes,
                                ReplayHeader&                 hdr,
                                std::vector<ReplayRecord>&    records,
                                std::uint64_t&                final_hash);

}  // namespace ironclad
