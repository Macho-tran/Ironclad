// SPDX-License-Identifier: MIT
//
// Transport interface for moving packets between peers.
//
// The session never talks to the network directly: it always goes
// through `ITransport`, which lets us drop in a deterministic
// loopback for tests + soak, or a real UDP transport for actual
// networked play. Packets are opaque byte spans; the session itself
// owns the schema (see `packet.hpp`).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ironclad {

struct RecvPacket {
    std::uint8_t              from_player = 0;
    std::vector<std::uint8_t> bytes;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    /// Send `bytes` from this peer to `to_player`. The transport must
    /// not retain the span beyond the call.
    virtual void send(std::uint8_t to_player,
                      std::span<const std::uint8_t> bytes) = 0;

    /// Returns the next available incoming packet, or `std::nullopt`
    /// if none is currently deliverable. Non-blocking.
    [[nodiscard]] virtual std::optional<RecvPacket> recv() = 0;

    /// Best-effort poll. Implementations like the loopback hub use
    /// this to advance their internal time / network simulator. Real
    /// UDP transports may treat it as a no-op.
    virtual void poll() {}
};

}  // namespace ironclad
