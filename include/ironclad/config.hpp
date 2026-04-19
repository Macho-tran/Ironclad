// SPDX-License-Identifier: MIT
// Compile-time tunables for the ironclad library.
// Keeping these in one place makes it easy for downstream users to override
// them with `-DIRONCLAD_FOO=...` on the compiler command line.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ironclad {

/// Library version, bumped on packet-schema or snapshot-format changes.
inline constexpr std::uint16_t kVersion = 1;

/// Default fixed-step simulation rate (Hz). Sessions may override.
#ifndef IRONCLAD_DEFAULT_TICK_HZ
#define IRONCLAD_DEFAULT_TICK_HZ 60
#endif
inline constexpr std::uint16_t kDefaultTickHz = IRONCLAD_DEFAULT_TICK_HZ;

/// Default rollback ring-buffer depth (frames). Must be a power of two for
/// fast wrap. 32 covers ~530 ms at 60 Hz which is generous for LAN/internet.
#ifndef IRONCLAD_DEFAULT_RING_SIZE
#define IRONCLAD_DEFAULT_RING_SIZE 32
#endif
inline constexpr std::uint16_t kDefaultRingSize = IRONCLAD_DEFAULT_RING_SIZE;
static_assert((kDefaultRingSize & (kDefaultRingSize - 1)) == 0,
              "ring size must be a power of two");

/// Maximum number of players supported in a single session. Compile-time so
/// per-player arrays can live in fixed-size storage.
#ifndef IRONCLAD_MAX_PLAYERS
#define IRONCLAD_MAX_PLAYERS 8
#endif
inline constexpr std::size_t kMaxPlayers = IRONCLAD_MAX_PLAYERS;

}  // namespace ironclad
