// SPDX-License-Identifier: MIT
//
// Deterministic in-process transport hub. N peers, every send goes
// through a per-link network simulator and is queued for delivery
// at a future tick. Used both by the unit tests and the headless
// arena_demo soak harness.
//
// Time is tracked as a tick counter incremented by `advance_tick`.
// Sessions call `transport.poll()` at the top of each tick and
// `advance_tick` after the tick is complete. This keeps the netsim
// reproducible and replay-friendly.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <vector>

#include "config.hpp"
#include "netsim.hpp"
#include "rng.hpp"
#include "transport.hpp"

namespace ironclad {

class LoopbackHub {
public:
    explicit LoopbackHub(std::uint8_t num_players,
                         NetSimConfig cfg = {})
        : num_players_(num_players),
          cfg_(cfg),
          rng_(cfg.seed),
          peers_(num_players) {
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            peers_[i].player_id = static_cast<std::uint8_t>(i);
            peers_[i].hub       = this;
        }
    }

    [[nodiscard]] std::uint8_t num_players() const noexcept { return num_players_; }
    [[nodiscard]] std::uint64_t current_tick() const noexcept { return tick_; }

    [[nodiscard]] ITransport* transport(std::uint8_t player) {
        return &peers_[player];
    }

    /// Advance the hub's clock by one tick. Packets whose deliver-tick
    /// has arrived become readable via the corresponding peer's
    /// `recv()`.
    void advance_tick() { ++tick_; }

    /// Live counters used by the diegetic stats overlay. Reset on
    /// `reset_stats`.
    struct Stats {
        std::uint64_t packets_sent     = 0;
        std::uint64_t packets_delivered = 0;
        std::uint64_t packets_dropped  = 0;
        std::uint64_t packets_reordered = 0;
        std::uint64_t bytes_sent       = 0;
        std::uint64_t bytes_delivered  = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }

    /// Run-time configuration tweaks (used by tests and the demo CLI).
    void set_config(NetSimConfig cfg) noexcept { cfg_ = cfg; }
    [[nodiscard]] const NetSimConfig& config() const noexcept { return cfg_; }

private:
    struct Pending {
        std::uint64_t              deliver_tick = 0;
        std::uint8_t               from_player  = 0;
        std::vector<std::uint8_t>  bytes;
    };

    struct Peer : ITransport {
        std::uint8_t      player_id = 0;
        LoopbackHub*      hub       = nullptr;
        // Inbox: packets ready to be delivered to this peer, ordered by
        // deliver_tick. We use a vector + sort-on-insert because the
        // queues stay tiny.
        std::vector<Pending> inbox;

        void send(std::uint8_t to_player,
                  std::span<const std::uint8_t> bytes) override {
            hub->enqueue(player_id, to_player, bytes);
        }

        std::optional<RecvPacket> recv() override {
            // Find the earliest deliverable entry.
            const std::uint64_t now = hub->tick_;
            std::size_t best = inbox.size();
            for (std::size_t i = 0; i < inbox.size(); ++i) {
                if (inbox[i].deliver_tick <= now) {
                    if (best == inbox.size() ||
                        inbox[i].deliver_tick < inbox[best].deliver_tick ||
                        (inbox[i].deliver_tick == inbox[best].deliver_tick &&
                         i < best)) {
                        best = i;
                    }
                }
            }
            if (best == inbox.size()) return std::nullopt;
            RecvPacket out;
            out.from_player = inbox[best].from_player;
            out.bytes       = std::move(inbox[best].bytes);
            // O(n) erase but n is tiny.
            inbox.erase(inbox.begin() + static_cast<std::ptrdiff_t>(best));
            ++hub->stats_.packets_delivered;
            hub->stats_.bytes_delivered += out.bytes.size();
            return out;
        }
    };

    void enqueue(std::uint8_t from, std::uint8_t to,
                 std::span<const std::uint8_t> bytes) {
        ++stats_.packets_sent;
        stats_.bytes_sent += bytes.size();

        // Loss check first.
        if (cfg_.loss_pct > 0 &&
            rng_.next_below(100u) < cfg_.loss_pct) {
            ++stats_.packets_dropped;
            return;
        }

        // Compute deliver tick = now + latency + uniform jitter.
        std::uint64_t delay = cfg_.latency_ticks;
        if (cfg_.jitter_ticks > 0) {
            delay += rng_.next_below(static_cast<std::uint32_t>(cfg_.jitter_ticks) + 1u);
        }

        Pending p;
        p.deliver_tick = tick_ + delay;
        p.from_player  = from;
        p.bytes.assign(bytes.begin(), bytes.end());

        auto& target = peers_[to].inbox;

        // Reorder check: bias the deliver tick of *this* packet so it
        // arrives a hair earlier or later than the last one queued.
        if (!target.empty() && cfg_.reorder_pct > 0 &&
            rng_.next_below(100u) < cfg_.reorder_pct) {
            // Swap with the most recent: copy its deliver_tick + 1
            // and move the most recent to deliver_tick + 0.
            auto& last = target.back();
            std::swap(p.deliver_tick, last.deliver_tick);
            ++stats_.packets_reordered;
        }

        target.push_back(std::move(p));
    }

    std::uint8_t       num_players_;
    NetSimConfig       cfg_;
    Rng                rng_;
    std::vector<Peer>  peers_;
    std::uint64_t      tick_ = 0;
    Stats              stats_{};
};

}  // namespace ironclad
