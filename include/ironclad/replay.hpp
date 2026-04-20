// SPDX-License-Identifier: MIT
//
// Replay Studio model — turns an .iclr byte buffer into an
// inspectable, time-scrubable representation of a recorded session.
//
// Two layers:
//
//   * `ReplayModel` parses the file (v1 or v2), exposes per-frame
//     records, derives a list of `RollbackEvent`s, and computes
//     summary `ReplayStats`. It does NOT instantiate any worlds.
//
//   * `Replayer` is the deterministic re-simulator. Given the same
//     init/step functions the original session used, it can rebuild
//     the `World` at any recorded frame. It uses a checkpoint cache
//     so scrubbing is O(distance from nearest checkpoint), not
//     O(frame).
//
// Determinism: `Replayer` is stateless w.r.t. anything outside the
// replay file plus the user-supplied init/step. The brief's
// canonical inputs are what was fed into the simulation when the
// recording was made; replaying them produces a byte-identical
// world for every frame, which means the recorded `hash` field is
// the authoritative checksum at any point.
//
// We deliberately do not use `ironclad::Session` for re-simulation:
// rollback / prediction / network are all irrelevant offline (the
// inputs are already canonical), and avoiding `Session` keeps the
// dependency graph clean and the replay path debuggable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include "ecs.hpp"
#include "input.hpp"
#include "recorder.hpp"
#include "rng.hpp"
#include "session.hpp"

namespace ironclad {

/// Frame-tagged event derived from the replay records. Today we
/// surface non-zero rollback distance and desync flags; future
/// kinds (e.g. lag-comp shots, projectile spawns) can be added
/// without breaking the wire format.
struct RollbackEvent {
    std::uint32_t frame    = 0;
    std::uint8_t  distance = 0;     // ROLLBACK FRAMES
    bool          desync   = false; // mirrored from flags bit 0
};

/// Summary of a recording, computed once at load.
struct ReplayStats {
    std::uint32_t frame_count          = 0;
    std::uint64_t total_rollback_frames = 0;
    double        avg_rollback_frames  = 0.0;
    std::uint8_t  max_rollback_frames  = 0;
    std::uint32_t rollback_event_count = 0;     // ticks with rollback > 0
    std::uint32_t desync_event_count   = 0;
    /// Histogram bucketed at [0,1,2,4,8,16,32,64+] frames.
    std::array<std::uint32_t, 8> rollback_histogram{};
};

/// Parsed, indexed, summary-augmented replay.
class ReplayModel {
public:
    /// Parse `bytes` into a ready-to-inspect model. Returns
    /// `std::nullopt` on any malformed input.
    [[nodiscard]] static std::optional<ReplayModel>
    load(std::span<const std::uint8_t> bytes);

    /// Convenience: load from a filesystem path. Returns
    /// `std::nullopt` if the file is missing or malformed.
    [[nodiscard]] static std::optional<ReplayModel>
    load_file(const std::string& path);

    [[nodiscard]] const ReplayHeader&                 header()        const noexcept { return hdr_; }
    [[nodiscard]] const std::vector<ReplayRecord>&    records()       const noexcept { return records_; }
    [[nodiscard]] const std::vector<RollbackEvent>&   events()        const noexcept { return events_; }
    [[nodiscard]] const std::vector<LagEvent>&        lag_events()    const noexcept { return lag_events_; }
    [[nodiscard]] const ReplayStats&                  stats()         const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t                       final_hash()    const noexcept { return final_hash_; }
    [[nodiscard]] std::uint32_t                       record_count()  const noexcept {
        return static_cast<std::uint32_t>(records_.size());
    }

    /// Returns the lag event closest to `frame` within `window`
    /// (in ticks), or nullptr if none found. Used by the studio
    /// to decide when to draw rewound hitboxes.
    [[nodiscard]] const LagEvent* nearest_lag_event(
        std::uint32_t frame, std::uint32_t window = 12) const noexcept;

    /// Index of the rollback event at or after `frame`, or
    /// `events().size()` if none. Wrap to 0 if `wrap` is true.
    [[nodiscard]] std::size_t next_event_index(std::uint32_t frame, bool wrap = false) const noexcept;

    /// Index of the rollback event at or before `frame`, or
    /// `events().size()` if none. Wrap to `size-1` if `wrap` is true.
    [[nodiscard]] std::size_t prev_event_index(std::uint32_t frame, bool wrap = false) const noexcept;

    /// Returns the per-record index for `frame`, or
    /// `record_count()` if not found. O(log n).
    [[nodiscard]] std::size_t record_index_for_frame(std::uint32_t frame) const noexcept;

private:
    ReplayHeader               hdr_{};
    std::vector<ReplayRecord>  records_;
    std::vector<LagEvent>      lag_events_;
    std::uint64_t              final_hash_ = 0;
    std::vector<RollbackEvent> events_;
    ReplayStats                stats_{};
};

/// Deterministic re-simulator on top of a `ReplayModel`. Caller
/// supplies the init / step functions that were used at record
/// time; nothing else is needed.
class Replayer {
public:
    using SimStep   = ironclad::Session::SimStep;
    using WorldInit = ironclad::Session::WorldInit;

    /// `model` must outlive the Replayer.
    Replayer(const ReplayModel& model,
             WorldInit          init,
             SimStep            step,
             std::uint16_t      checkpoint_interval = 60);

    /// Returns a const reference to the world rebuilt for `frame`.
    /// `frame == 0` returns the initial world (post-init, pre-tick).
    /// Out-of-range frames clamp to the recording's last frame.
    /// Successive calls with monotonically advancing frames are
    /// O(1) amortized; arbitrary scrubs are O(checkpoint_interval).
    [[nodiscard]] const World& world_at(std::uint32_t frame);

    /// Validate that re-simulation produces every recorded hash.
    /// Returns the first divergent frame, or `kNoDivergence` if
    /// the chain is intact end-to-end.
    static constexpr std::uint32_t kNoDivergence = std::numeric_limits<std::uint32_t>::max();
    [[nodiscard]] std::uint32_t validate_hash_chain();

private:
    void rebuild_to(std::uint32_t frame);
    void capture_checkpoint(std::uint32_t frame);
    void load_checkpoint(std::uint32_t frame);

    const ReplayModel& model_;
    WorldInit          init_;
    SimStep            step_;
    std::uint16_t      checkpoint_interval_;

    World              world_;
    Rng                rng_;
    std::uint32_t      current_frame_ = 0;

    struct Checkpoint {
        std::uint32_t              frame = 0;
        std::vector<std::uint8_t>  bytes;     // serialized world
        std::uint64_t              rng_state = 0;
    };
    std::vector<Checkpoint> checkpoints_;
};

}  // namespace ironclad
