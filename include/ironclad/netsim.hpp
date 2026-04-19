// SPDX-License-Identifier: MIT
//
// Network simulator. Models per-link latency, jitter, packet loss,
// and reordering using its own seeded RNG (separate from the sim
// RNG so changing netsim parameters never affects determinism of
// the simulation itself — only of the *delivery schedule*).
//
// Time is in simulation ticks rather than wall clock: the loopback
// hub increments a tick counter, and the netsim queues packets with
// "deliver at tick N" timestamps. This keeps the entire system
// reproducible from a single seed.
#pragma once

#include <cstdint>
#include <vector>

#include "rng.hpp"

namespace ironclad {

struct NetSimConfig {
    /// Mean one-way latency in *ticks*. At 60 Hz, 9 ticks ~= 150 ms.
    std::uint16_t latency_ticks = 0;
    /// Maximum jitter added (uniform 0..jitter_ticks) on top of latency.
    std::uint16_t jitter_ticks  = 0;
    /// Probability of dropping a packet, 0..100.
    std::uint8_t  loss_pct      = 0;
    /// Probability of reordering — implemented as "swap with the
    /// previous in-flight packet on this link" before delivery.
    std::uint8_t  reorder_pct   = 0;
    /// Seed for the *netsim* RNG. Must NOT be the same seed as the
    /// session: changing netsim parameters should not affect the
    /// sim RNG.
    std::uint64_t seed          = 0xDEADBEEF'13371337ULL;
};

}  // namespace ironclad
