// SPDX-License-Identifier: MIT
//
// `arena_udp` mode: runs a single Session on a real UDP socket,
// AI-driven. Used for the two-process demo:
//
//   Terminal A:  arena_demo --net listen --port 7777 --players 2
//                           --my-id 0 --frames 1800
//   Terminal B:  arena_demo --net connect --remote 127.0.0.1:7777
//                           --port 7778 --players 2 --my-id 1
//                           --frames 1800
//
// Both processes converge on byte-identical state; the host (or
// either side) can pass --record PATH to write an .iclr file
// directly from the live UDP traffic.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arena_demo {

struct UdpOptions {
    enum class Role { Listen, Connect };
    Role          role          = Role::Listen;
    std::uint16_t bind_port     = 0;
    std::vector<std::string> remotes;     // host:port strings; one per peer
    std::uint8_t  num_players   = 2;
    std::uint8_t  my_id         = 0;
    std::uint32_t frames        = 1800;
    std::uint64_t seed          = 0xC0FFEE'BEEF'D00DULL;
    const char*   record_path   = nullptr;
    bool          quiet         = false;
};

/// Returns 0 on success (no desync), 1 otherwise.
[[nodiscard]] int run_udp(const UdpOptions& opts);

}  // namespace arena_demo
