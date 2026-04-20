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
// === v3 ("IRCL_REPLAY3") — adds lag-comp event stream ===
//   Header identical to v2 (including version byte = 3 written into
//   the file's u16 version field).
//   After the per-frame records but BEFORE the trailer, an extra
//   block:
//       u32 lag_event_count
//       repeat lag_event_count times:
//           u32 frame                 — sim frame the shot landed
//           u8  attacker_id
//           u8  target_id             — 0xFF if no hit
//           u16 rewound_ticks         — rtt/2 in ticks
//           i64 origin_x_raw          — Q32.32 raw bits
//           i64 origin_y_raw
//           i64 dir_x_raw
//           i64 dir_y_raw
//           i64 range_raw
//   Trailer is the same "ENDR" + frame_count + final_hash as before.
//
//   v3 captures lag-compensated hit-scan events so the Replay Studio
//   can render rewound hitboxes when scrubbed near a shot frame.
//
// The recorder is intended to be wrapped *around* a Session: every
// `tick`, the host calls `recorder.record_v2(...)` (or the legacy
// `record(...)` which writes v3 records with zero rollback / flags
// / pred_diff — still a valid v3 file). Lag events are added via
// `record_lag_event(...)` and emitted at `finish()` time.
#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "fixed.hpp"
#include "input.hpp"
#include "vec2.hpp"

namespace ironclad {

/// A single lag-compensated hit-scan event recorded for the Replay
/// Studio. All geometry is in Q32.32 fixed-point so the studio can
/// render rewound positions identically to the simulation.
struct LagEvent {
    std::uint32_t frame          = 0;
    std::uint8_t  attacker_id    = 0;
    std::uint8_t  target_id      = 0xFFu;     // 0xFF = miss
    std::uint16_t rewound_ticks  = 0;
    Vec2          origin{};
    Vec2          dir{};
    Fixed         range          {};
};

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

    /// Append a lag-compensated hit-scan event. Buffered until
    /// `finish()` writes them all in one block before the trailer.
    void record_lag_event(const LagEvent& ev);

    /// Finalize and return the complete recording bytes.
    std::vector<std::uint8_t> finish(std::uint64_t final_hash);

    [[nodiscard]] std::uint32_t frame_count()    const noexcept { return frames_; }
    [[nodiscard]] std::size_t   bytes_written()  const noexcept { return out_.size(); }

private:
    std::vector<std::uint8_t> out_;
    std::uint8_t              num_players_ = 0;
    std::uint32_t             frames_      = 0;
    std::vector<LagEvent>     lag_events_;
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

/// Parses a recording. Returns true on success. Detects v1, v2, or
/// v3 from the magic bytes; older versions load with the unsupported
/// fields (rollback / flags / pred_diff for v1; lag events for
/// v1/v2) left empty.
[[nodiscard]] bool parse_replay(std::span<const std::uint8_t> bytes,
                                ReplayHeader&                 hdr,
                                std::vector<ReplayRecord>&    records,
                                std::vector<LagEvent>&        lag_events,
                                std::uint64_t&                final_hash);

/// Backwards-compatible overload that ignores the lag-event lane.
/// Existing callers don't need to change; new ones should prefer
/// the four-output form above.
[[nodiscard]] inline bool parse_replay(std::span<const std::uint8_t> bytes,
                                       ReplayHeader&                 hdr,
                                       std::vector<ReplayRecord>&    records,
                                       std::uint64_t&                final_hash) {
    std::vector<LagEvent> ignored;
    return parse_replay(bytes, hdr, records, ignored, final_hash);
}

}  // namespace ironclad
