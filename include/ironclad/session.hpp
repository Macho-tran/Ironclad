// SPDX-License-Identifier: MIT
//
// Public Session API. A `Session` owns one peer's view of a
// distributed simulation: its own copy of the world, the rollback
// state, the per-player input buffers, the snapshot ring, and the
// network simulator-backed transport.
//
// Lifecycle:
//   1. `Session::create(config, transport, world_init)` — registers
//      components and seeds the world; transport must already be
//      connected to `config.num_players` peers.
//   2. Per fixed-step tick, the host calls `tick(local_input)`.
//      Internally, the session:
//        a) Drains transport: remote inputs arrive, oldest divergent
//           frame is recorded.
//        b) Reconciles: if a divergence was found, the world is
//           restored from the snapshot just before that frame and
//           replayed forward using the corrected inputs.
//        c) Predicts missing remote inputs (repeat last known).
//        d) Steps the simulation one frame.
//        e) Snapshots & hashes the new state.
//        f) Emits an `InputPacket` to every other peer.
//
// Determinism is preserved by:
//   * The simulation reading inputs only from the per-player input
//     ring, never from anything else.
//   * The systems running in a fixed order each tick.
//   * The RNG state being part of the snapshot.
//   * The netsim using a *separate* RNG; changing its seed never
//     affects sim hashes, only delivery schedule.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "components.hpp"
#include "config.hpp"
#include "ecs.hpp"
#include "hash.hpp"
#include "input.hpp"
#include "rng.hpp"
#include "snapshot.hpp"
#include "transport.hpp"

namespace ironclad {

struct SessionConfig {
    std::uint8_t  num_players       = 2;
    std::uint8_t  local_player      = 0;
    std::uint16_t tick_hz           = kDefaultTickHz;
    std::uint16_t snapshot_ring     = kDefaultRingSize;
    std::uint64_t seed              = 0xC0FFEE'CAFE'D00DULL;
    /// World capacity in entities.
    std::uint32_t world_capacity    = 256;
    /// Local input delay in *ticks*. The local player's input pressed
    /// at tick T is applied to the simulation at tick T + delay. This
    /// is the classic rollback technique for trading a tiny bit of
    /// felt input latency for dramatically smaller average rollback
    /// distance. Typical values: 1..3.
    std::uint8_t  local_input_delay = 2;
};

/// Per-tick stats exposed to the diegetic overlay.
struct SessionStats {
    std::uint32_t current_frame           = 0;
    std::uint8_t  last_rollback_frames    = 0;
    std::uint64_t last_state_hash         = 0;
    std::uint64_t total_rollback_frames   = 0;
    std::uint64_t total_ticks             = 0;
    std::uint64_t bytes_sent              = 0;
    bool          desync_detected         = false;
    std::uint32_t desync_frame            = 0;
    std::uint64_t desync_local_hash       = 0;
    std::uint64_t desync_remote_hash      = 0;
};

class Session {
public:
    using SimStep   = std::function<void(World&, const PlayerInput*, std::uint8_t, Rng&)>;
    using WorldInit = std::function<void(World&, Rng&, const SessionConfig&)>;

    Session(SessionConfig    config,
            std::unique_ptr<ITransport> transport,
            WorldInit        world_init,
            SimStep          step);

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // ----- Per-tick driver ----------------------------------------------
    void tick(PlayerInput local_input);

    // ----- Inspection ----------------------------------------------------
    [[nodiscard]] const SessionStats& stats()  const noexcept { return stats_; }
    [[nodiscard]] const World&        world()  const noexcept { return world_; }
    [[nodiscard]] World&              world()        noexcept { return world_; }
    [[nodiscard]] std::uint32_t       current_frame() const noexcept { return frame_; }
    [[nodiscard]] std::uint64_t       state_hash()    const noexcept { return last_hash_; }

    // ----- Replay/test helpers ------------------------------------------
    /// Returns a copy of the local player's input for a given frame, or
    /// `std::nullopt` if it's outside the ring.
    std::optional<PlayerInput> input_for(std::uint8_t player, std::uint32_t frame) const;

    /// Returns the snapshot bytes for `frame`, or empty span if absent.
    std::span<const std::uint8_t> snapshot_for(std::uint32_t frame) const;

    /// True if the local player has authoritative inputs for the given
    /// frame from every peer.
    bool fully_acked(std::uint32_t frame) const;

private:
    void                step_one(std::uint32_t frame);
    void                save_snapshot(std::uint32_t frame);
    bool                load_snapshot(std::uint32_t frame);
    void                broadcast_input_packet();
    void                drain_transport();
    PlayerInput         input_for_step(std::uint8_t player, std::uint32_t frame) const;
    std::uint32_t       process_remote_input(std::uint8_t player,
                                             std::uint32_t frame,
                                             PlayerInput in);

    SessionConfig                          config_;
    std::unique_ptr<ITransport>            transport_;
    SimStep                                step_;
    World                                  world_;
    Rng                                    rng_;
    SnapshotRing                           snapshots_;
    std::vector<std::vector<PlayerInput>>  inputs_;        // per player, flat ring
    std::vector<std::vector<std::uint8_t>> input_valid_;   // parallel valid bits
    std::uint32_t                          frame_     = 0;
    std::uint64_t                          last_hash_ = 0;
    /// Last frame for which a non-predicted (i.e. peer-supplied) input
    /// is in our ring, per player. We use this to produce predictions.
    std::vector<std::uint32_t>             last_known_input_frame_;
    /// Most recently verified peer hash + frame (used in InputPackets).
    std::uint32_t                          ack_frame_ = 0;
    std::uint64_t                          ack_hash_  = 0;
    SessionStats                           stats_{};
};

}  // namespace ironclad
