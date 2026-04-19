// SPDX-License-Identifier: MIT
//
// Headless arena_demo: N AI players sharing a `LoopbackHub`, each
// with its own `Session`. This is the soak harness used by CI. It
// also doubles as the "demo runner" used to measure the KPIs.
//
// Outputs:
//   * stdout: per-second diegetic stats line.
//   * exit code 0 if no desync detected, 1 otherwise.
//   * optional `--record PATH` writes a .iclr replay of player 0.
//
// CLI:
//   arena_demo [--frames N] [--players P]
//              [--rtt-ms M] [--jitter-ms J]
//              [--loss-pct L] [--reorder-pct R]
//              [--seed 0xC0FFEE]
//              [--record PATH] [--quiet]
//
// All netsim parameters are deterministic given the seed; results
// reproduce byte-for-byte across runs of the same binary.
#pragma once

#include <cstdint>
#include <span>

#include <ironclad/components.hpp>
#include <ironclad/loopback_transport.hpp>
#include <ironclad/session.hpp>

namespace arena_demo {

struct Options {
    std::uint32_t frames        = 3600;     // 60 s @ 60 Hz
    std::uint8_t  num_players   = 4;
    std::uint16_t rtt_ms        = 0;
    std::uint16_t jitter_ms     = 0;
    std::uint8_t  loss_pct      = 0;
    std::uint8_t  reorder_pct   = 0;
    std::uint64_t seed          = 0xC0FFEE'BEEF'D00DULL;
    const char*   record_path   = nullptr;
    bool          quiet         = false;
};

struct Result {
    bool          ok                       = false;
    std::uint64_t total_ticks              = 0;
    std::uint64_t total_rollback_frames    = 0;
    double        avg_rollback_frames      = 0.0;
    std::uint64_t bytes_sent_per_client    = 0;
    double        bandwidth_kbps_per_client = 0.0;
    bool          desync_detected          = false;
};

[[nodiscard]] Result run(const Options&);

void step_arena(ironclad::World& world,
                const ironclad::PlayerInput* inputs,
                std::uint8_t num_players,
                ironclad::Rng& rng);

void init_arena(ironclad::World& world,
                ironclad::Rng& rng,
                const ironclad::SessionConfig& cfg);

ironclad::PlayerInput ai_input(std::uint32_t frame, std::uint8_t player);

}  // namespace arena_demo
