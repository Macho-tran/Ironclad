// SPDX-License-Identifier: MIT
//
// UDP transport implementation. Single-byte HELLO frame protocol
// in front of the regular Session bytes:
//
//   first byte = 0x49  ('I')   ironclad data frame, payload follows
//   first byte = 0x48  ('H')   HELLO, payload = [u8 sender_player_id]
//
// HELLO is what lets a client reach the host on a NAT-friendly port
// (it punches an inbound 5-tuple) and what teaches the host where
// the client lives so it can reply. We send HELLO opportunistically
// before every data packet for the first ~1s of a session.
#include <ironclad/udp_transport.hpp>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
using ssize_t   = int;
#  define IRONCLAD_BAD_SOCK INVALID_SOCKET
#  define ironclad_close_socket closesocket
#  define ironclad_last_err()    ::WSAGetLastError()
inline bool ironclad_would_block(int e) { return e == WSAEWOULDBLOCK; }
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define IRONCLAD_BAD_SOCK (-1)
#  define ironclad_close_socket ::close
#  define ironclad_last_err()    errno
inline bool ironclad_would_block(int e) {
    // On every modern POSIX system EAGAIN == EWOULDBLOCK, but we
    // accept either explicitly (the C standard permits them being
    // distinct). Comparing both also reads as more obviously
    // correct to a reader who doesn't already know they alias.
#if EAGAIN == EWOULDBLOCK
    return e == EAGAIN;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}
#endif

namespace ironclad {

namespace {

constexpr std::uint8_t kFrameData  = 'I';
constexpr std::uint8_t kFrameHello = 'H';
constexpr std::size_t  kMaxPacket  = 1500;     // ~MTU

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { WSACleanup(); }
};
WinsockInit& winsock() {
    static WinsockInit w;
    return w;
}
#endif

void set_nonblocking(int sock) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

}  // namespace

std::optional<UdpEndpoint> UdpEndpoint::parse(const std::string& host_port) {
    auto colon = host_port.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string host = host_port.substr(0, colon);
    std::string port = host_port.substr(colon + 1);
    if (host.empty()) host = "127.0.0.1";

    in_addr addr{};
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) return std::nullopt;

    char* end = nullptr;
    unsigned long pn = std::strtoul(port.c_str(), &end, 10);
    if (!end || *end != 0 || pn == 0 || pn > 0xFFFFu) return std::nullopt;

    UdpEndpoint ep;
    ep.ipv4 = ntohl(addr.s_addr);
    ep.port = static_cast<std::uint16_t>(pn);
    return ep;
}

std::string UdpEndpoint::to_string() const {
    in_addr a;
    a.s_addr = htonl(ipv4);
    char buf[64];
    ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(static_cast<int>(port));
}

UdpTransport::UdpTransport(std::uint16_t bind_port, std::uint8_t local_player)
    : local_player_(local_player) {
#if defined(_WIN32)
    (void)winsock();   // ensure WSAStartup
#endif
    int s = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (s == IRONCLAD_BAD_SOCK) {
        throw std::runtime_error("UdpTransport: socket() failed");
    }
    sock_ = s;
    set_nonblocking(sock_);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(bind_port);
    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ironclad_close_socket(sock_);
        sock_ = -1;
        throw std::runtime_error("UdpTransport: bind() failed");
    }
    sockaddr_in actual{};
    socklen_t   alen = sizeof(actual);
    if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
        bound_port_ = ntohs(actual.sin_port);
    } else {
        bound_port_ = bind_port;
    }
}

UdpTransport::~UdpTransport() {
    if (sock_ >= 0) ironclad_close_socket(sock_);
}

void UdpTransport::add_peer(std::uint8_t peer_id, UdpEndpoint ep) {
    for (auto& p : peers_) {
        if (p.id == peer_id) { p.ep = ep; return; }
    }
    peers_.push_back({peer_id, ep});
    // Punch the inbound 5-tuple immediately so the peer can reply
    // even if we're behind NAT.
    send_hello_to(ep);
}

void UdpTransport::rebroadcast_hello() {
    for (const auto& p : peers_) {
        send_hello_to(p.ep);
    }
}

bool UdpTransport::all_peers_ready(std::uint8_t num_players) const noexcept {
    for (std::uint8_t p = 0; p < num_players; ++p) {
        if (p == local_player_) continue;
        bool found = false;
        for (const auto& e : peers_) if (e.id == p) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

void UdpTransport::send_hello_to(const UdpEndpoint& ep) {
    std::uint8_t buf[2] = { kFrameHello, local_player_ };
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(ep.ipv4);
    dst.sin_port        = htons(ep.port);
    (void)::sendto(sock_, reinterpret_cast<const char*>(buf), 2, 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

void UdpTransport::send(std::uint8_t to_player,
                        std::span<const std::uint8_t> bytes) {
    UdpEndpoint dst_ep{};
    bool found = false;
    for (const auto& p : peers_) {
        if (p.id == to_player) { dst_ep = p.ep; found = true; break; }
    }
    if (!found) return;     // unknown peer — silently drop

    if (bytes.size() + 1 > kMaxPacket) return;

    // Frame: data byte + payload.
    std::uint8_t frame[kMaxPacket];
    frame[0] = kFrameData;
    std::memcpy(frame + 1, bytes.data(), bytes.size());

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(dst_ep.ipv4);
    dst.sin_port        = htons(dst_ep.port);
    const std::size_t send_len = bytes.size() + 1;
#if defined(_WIN32)
    (void)::sendto(sock_, reinterpret_cast<const char*>(frame),
                   static_cast<int>(send_len), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
#else
    (void)::sendto(sock_, frame, send_len, 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
#endif
    ++stats_.packets_sent;
}

std::optional<RecvPacket> UdpTransport::recv() {
    while (true) {
        std::uint8_t buf[kMaxPacket];
        sockaddr_in  src{};
        socklen_t    slen = sizeof(src);
#if defined(_WIN32)
        int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf),
                           static_cast<int>(sizeof(buf)), 0,
                           reinterpret_cast<sockaddr*>(&src), &slen);
#else
        ssize_t n = ::recvfrom(sock_, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
#endif
        if (n < 0) {
            // WOULDBLOCK is "no packet right now" — totally normal.
            // For anything else there's nothing actionable in a
            // best-effort transport, so we signal the caller the
            // same way and let the next tick try again.
            (void)ironclad_would_block(ironclad_last_err());
            return std::nullopt;
        }
        if (n < 1) continue;

        UdpEndpoint from;
        from.ipv4 = ntohl(src.sin_addr.s_addr);
        from.port = ntohs(src.sin_port);

        if (buf[0] == kFrameHello) {
            ++stats_.hellos_received;
            if (n >= 2) {
                std::uint8_t pid = buf[1];
                add_peer(pid, from);
            }
            continue;       // not a data packet; loop
        }
        if (buf[0] != kFrameData) {
            ++stats_.bad_packets_dropped;
            continue;
        }

        // Look up which peer this came from.
        std::uint8_t from_player = 0xFF;
        for (const auto& p : peers_) {
            if (p.ep == from) { from_player = p.id; break; }
        }
        if (from_player == 0xFF) {
            // Unknown sender; ignore (in practice a HELLO will
            // teach us about them shortly).
            ++stats_.bad_packets_dropped;
            continue;
        }

        ++stats_.packets_received;
        RecvPacket out;
        out.from_player = from_player;
        out.bytes.assign(buf + 1, buf + n);
        return out;
    }
}

}  // namespace ironclad
