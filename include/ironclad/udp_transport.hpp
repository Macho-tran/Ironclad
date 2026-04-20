// SPDX-License-Identifier: MIT
//
// UDP transport for Ironclad sessions.
//
// Design choices:
//   * Best-effort delivery only. Every packet we exchange is an
//     `InputPacket` containing the most recent N inputs, so a lost
//     packet just means the receiver may have to predict for an
//     extra frame and roll back when the next one arrives. We do
//     NOT add reliability/retransmit on top — that's both wasted
//     bandwidth and a determinism foot-gun.
//   * Per-peer endpoint table keyed by player id. Endpoints are
//     learned in two ways:
//       - explicit constructor argument (`UdpTransport::add_peer`),
//       - first inbound packet from a previously-unknown peer
//         that includes a HELLO byte.
//   * No threading. The Session-tick loop drains packets via
//     `recv()` itself; `poll()` is a no-op so this transport can
//     be swapped in for `LoopbackHub::transport(p)` with no other
//     code change.
//   * POSIX (Linux/macOS) and Windows. The Winsock shim lives in a
//     small private header so the public API is identical.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "transport.hpp"

namespace ironclad {

/// IPv4 endpoint (host:port). We deliberately stay v4-only for now;
/// gameplay traffic is fine on v4 and IPv6 dual-stack adds gnarly
/// per-OS behavior (`IPV6_V6ONLY` defaults differ).
struct UdpEndpoint {
    std::uint32_t ipv4 = 0;     // host byte order
    std::uint16_t port = 0;

    [[nodiscard]] static std::optional<UdpEndpoint>
    parse(const std::string& host_port);
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const UdpEndpoint& a, const UdpEndpoint& b) noexcept {
        return a.ipv4 == b.ipv4 && a.port == b.port;
    }
};

class UdpTransport : public ITransport {
public:
    /// Construct bound to `bind_port` (0 = ephemeral). `local_player`
    /// identifies us to peers via the HELLO byte. Throws on bind
    /// failure.
    UdpTransport(std::uint16_t bind_port, std::uint8_t local_player);
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&)            = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /// Register a peer. Required for outbound `send()` to know
    /// where to address `to_player`. May be supplemented at runtime
    /// when an inbound HELLO arrives from a new peer.
    void add_peer(std::uint8_t peer_id, UdpEndpoint ep);

    /// The port this socket is actually bound to (useful when
    /// `bind_port == 0`).
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }

    /// True if every player in `[0, num_players)` (excluding our
    /// own id) has had its endpoint registered. Useful for a
    /// session host to wait for everyone to connect before starting.
    [[nodiscard]] bool all_peers_ready(std::uint8_t num_players) const noexcept;

    /// Re-emit a HELLO frame to every currently-known peer. Useful
    /// during the discovery handshake to keep punching the inbound
    /// 5-tuple while we wait for stragglers.
    void rebroadcast_hello();

    /// Diagnostic counters. Reset only on construction.
    struct Stats {
        std::uint64_t packets_sent          = 0;
        std::uint64_t packets_received      = 0;
        std::uint64_t hellos_received       = 0;
        std::uint64_t bad_packets_dropped   = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    // ITransport
    void send(std::uint8_t to_player,
              std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] std::optional<RecvPacket> recv() override;
    void poll() override {}

private:
    void send_hello_to(const UdpEndpoint& ep);

    int            sock_         = -1;
    std::uint16_t  bound_port_   = 0;
    std::uint8_t   local_player_ = 0;

    struct PeerEntry { std::uint8_t id; UdpEndpoint ep; };
    std::vector<PeerEntry> peers_;
    Stats                  stats_{};
};

}  // namespace ironclad
