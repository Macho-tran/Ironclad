// SPDX-License-Identifier: MIT
#include "arena_udp.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

#include <ironclad/components.hpp>
#include <ironclad/recorder.hpp>
#include <ironclad/session.hpp>
#include <ironclad/udp_transport.hpp>

#include "arena.hpp"

namespace arena_demo {

using namespace ironclad;

int run_udp(const UdpOptions& o) {
    if (o.my_id >= o.num_players) {
        std::fprintf(stderr, "--my-id must be < --players\n");
        return 2;
    }

    auto udp = std::make_unique<UdpTransport>(o.bind_port, o.my_id);
    std::printf("ironclad/udp: bound on port %u as player %u of %u\n",
                static_cast<unsigned>(udp->bound_port()),
                static_cast<unsigned>(o.my_id),
                static_cast<unsigned>(o.num_players));

    // Connect mode: --remote host:port[,...] gives us peer endpoints
    // for the IDs that are NOT our own, in ascending id order.
    if (o.role == UdpOptions::Role::Connect) {
        std::size_t i = 0;
        for (std::uint8_t p = 0; p < o.num_players; ++p) {
            if (p == o.my_id) continue;
            if (i >= o.remotes.size()) {
                std::fprintf(stderr,
                    "not enough --remote entries (need %u, got %zu)\n",
                    static_cast<unsigned>(o.num_players - 1),
                    o.remotes.size());
                return 2;
            }
            auto ep = UdpEndpoint::parse(o.remotes[i]);
            if (!ep) {
                std::fprintf(stderr, "bad --remote: %s\n", o.remotes[i].c_str());
                return 2;
            }
            udp->add_peer(p, *ep);
            std::printf("  peer %u -> %s\n",
                        static_cast<unsigned>(p), ep->to_string().c_str());
            ++i;
        }
    }

    // Wait up to 5 s for every peer to introduce itself via HELLO.
    // We rebroadcast HELLO every 100 ms because the listening side
    // has no a-priori knowledge of who its peers are; it learns
    // their addresses only when their first HELLO arrives.
    {
        std::printf("waiting for %u peers...\n",
                    static_cast<unsigned>(o.num_players - 1));
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        auto next_hello = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            (void)udp->recv();      // pumps the HELLO handler
            if (udp->all_peers_ready(o.num_players)) break;
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_hello) {
                udp->rebroadcast_hello();
                next_hello = now + std::chrono::milliseconds(100);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!udp->all_peers_ready(o.num_players)) {
            std::fprintf(stderr,
                "timeout waiting for peers (got introduction from a subset)\n");
            // fall through anyway — best-effort
        } else {
            std::printf("all peers ready, starting simulation\n");
        }
    }

    SessionConfig sc;
    sc.num_players       = o.num_players;
    sc.local_player      = o.my_id;
    sc.tick_hz           = kDefaultTickHz;
    sc.seed              = o.seed;
    sc.local_input_delay = 0;     // recording-friendly

    UdpTransport* raw_udp = udp.get();
    Session session(sc, std::move(udp),
                    /*world_init*/ init_arena,
                    /*step*/       step_arena);

    Recorder rec;
    if (o.record_path) {
        auto initial = session.snapshot_for(0);
        rec.begin(kDefaultTickHz, o.num_players, o.seed, 256,
                  {initial.data(), initial.size()});
    }

    constexpr std::chrono::microseconds kTickPeriod{16667};   // ~60Hz
    auto next_tick = std::chrono::steady_clock::now();
    for (std::uint32_t f = 0; f < o.frames; ++f) {
        // AI input from this peer's perspective only.
        PlayerInput in = ai_input(f, o.my_id);
        session.tick(in);

        if (o.record_path) {
            // Best-effort recorder: we know our own input is canonical;
            // for remote players, ask the session for whatever it now
            // has authoritatively (post-tick). After several seconds
            // these settle to the AI-canonical values.
            std::vector<PlayerInput> applied(o.num_players);
            for (std::uint8_t p = 0; p < o.num_players; ++p) {
                applied[p] = (p == o.my_id) ? in :
                    session.input_for(p, f).value_or(PlayerInput{});
            }
            rec.record_v2(f, applied,
                          session.stats().last_state_hash,
                          session.stats().last_rollback_frames,
                          session.stats().desync_detected
                              ? ReplayRecord::kFlagDesync : 0,
                          /*pred_diff=*/0);
        }

        if (!o.quiet && (f % kDefaultTickHz) == 0) {
            const auto& s = session.stats();
            const auto& us = raw_udp->stats();
            std::printf("[t=%5.1fs] f=%u rollback_total=%llu hash=%016llx "
                        "udp_sent=%llu udp_recv=%llu desync=%s\n",
                        static_cast<double>(f) / kDefaultTickHz,
                        f,
                        static_cast<unsigned long long>(s.total_rollback_frames),
                        static_cast<unsigned long long>(s.last_state_hash),
                        static_cast<unsigned long long>(us.packets_sent),
                        static_cast<unsigned long long>(us.packets_received),
                        s.desync_detected ? "YES" : "no");
            std::fflush(stdout);
        }
        next_tick += kTickPeriod;
        std::this_thread::sleep_until(next_tick);
    }

    if (o.record_path) {
        auto bytes = rec.finish(session.stats().last_state_hash);
        std::ofstream out(o.record_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        std::printf("wrote %s (%zu bytes)\n", o.record_path, bytes.size());
    }

    if (session.stats().desync_detected) {
        std::fprintf(stderr, "DESYNC DETECTED at frame %u\n",
                     session.stats().desync_frame);
        return 1;
    }
    return 0;
}

}  // namespace arena_demo
