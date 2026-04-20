// SPDX-License-Identifier: MIT
//
// Replay recorder & player.
//
// File format (.iclr) — all little-endian. Two versions exist; the
// parser auto-detects via the magic bytes.
//
// === v1 ("IRCL_REPLAY1") — original format ===
//   magic        : "IRCL_REPLAY1"  (12 bytes incl. NUL)
//   version      : u16             (== 1)
//   tick_hz      : u16
//   num_players  : u8
//   reserved     : u8 (must be 0)
//   seed         : u64
//   world_cap    : u32
//   init_size    : u32
//   init_bytes   : u8[init_size]
//   <records v1>...
//   trailer      : "ENDR" + u32 frame_count + u64 final_hash
//
//   Each v1 record: u32 frame, PlayerInput[num_players], u64 hash
//
// === v2 ("IRCL_REPLAY2") — Replay Studio format ===
//   Identical header except magic is "IRCL_REPLAY2" and version=2.
//   Each record adds three trailing bytes after the v1 payload:
//       u8 rollback   — frames rolled back on this tick (0..255)
//       u8 flags      — bit 0: desync detected on this tick
//       u8 pred_diff  — bitmask: bit P set if some peer's
//                       prediction for player P diverged from the
//                       canonical input on this frame
//   Trailer is unchanged.
//
// The recorder is intended to be wrapped *around* a Session: every
// `tick`, the host calls `recorder.record_v2(...)` (or the legacy
// `record(...)` which writes v2 records with zero rollback / flags
// / pred_diff — still a valid v2 file).
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
    /// bytes immediately after `init_arena`. Always writes a
    /// v2-format file.
    void begin(std::uint16_t tick_hz,
               std::uint8_t  num_players,
               std::uint64_t seed,
               std::uint32_t world_capacity,
               std::span<const std::uint8_t> initial_snapshot);

    /// Legacy / minimal record: zero rollback, zero flags, zero
    /// pred_diff. Equivalent to `record_v2(frame, inputs, hash, 0,
    /// 0, 0)`.
    void record(std::uint32_t frame,
                std::span<const PlayerInput> per_player_inputs,
                std::uint64_t hash);

    /// Full v2 record. `rollback` is the number of frames rolled
    /// back on this tick by the worst-affected peer. `flags` bit 0
    /// is set when a peer detected a desync this tick.
    /// `pred_diff` is a bitmask: bit P is set if at least one peer
    /// predicted a different input for player P on this frame than
    /// the canonical input fed into the simulation.
    void record_v2(std::uint32_t frame,
                   std::span<const PlayerInput> per_player_inputs,
                   std::uint64_t hash,
                   std::uint8_t  rollback,
                   std::uint8_t  flags,
                   std::uint8_t  pred_diff);

    /// Finalize and return the complete recording bytes.
    std::vector<std::uint8_t> finish(std::uint64_t final_hash);

    [[nodiscard]] std::uint32_t frame_count()    const noexcept { return frames_; }
    [[nodiscard]] std::size_t   bytes_written()  const noexcept { return out_.size(); }

private:
    std::vector<std::uint8_t> out_;
    std::uint8_t              num_players_ = 0;
    std::uint32_t             frames_      = 0;
};

/// Header info parsed from a recording file.
struct ReplayHeader {
    std::uint16_t              version          = 0;     // 1 or 2
    std::uint16_t              tick_hz          = 0;
    std::uint8_t               num_players      = 0;
    std::uint64_t              seed             = 0;
    std::uint32_t              world_capacity   = 0;
    std::vector<std::uint8_t>  initial_snapshot;
};

/// Per-frame record. Fields beyond `hash` are zero for v1 inputs.
struct ReplayRecord {
    std::uint32_t              frame      = 0;
    std::vector<PlayerInput>   inputs;
    std::uint64_t              hash       = 0;
    std::uint8_t               rollback   = 0;     // v2 only
    std::uint8_t               flags      = 0;     // v2 only; bit 0 = desync
    std::uint8_t               pred_diff  = 0;     // v2 only
    static constexpr std::uint8_t kFlagDesync = 1u << 0;
};

/// Parses a recording. Returns true on success. Detects v1 or v2
/// from the magic bytes; v1 files load with rollback / flags /
/// pred_diff = 0 on every record.
[[nodiscard]] bool parse_replay(std::span<const std::uint8_t> bytes,
                                ReplayHeader&                 hdr,
                                std::vector<ReplayRecord>&    records,
                                std::uint64_t&                final_hash);

}  // namespace ironclad
