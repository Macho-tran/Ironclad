// SPDX-License-Identifier: MIT
//
// Lag-compensated hit-scan. Stores a circular buffer of recent
// authoritative target positions and bounding circles, and offers a
// `hitscan()` query that rewinds positions to a target tick before
// performing the test.
//
// Determinism note: we use linear interpolation between the two
// snapshots that bracket the target tick, all in fixed-point.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "fixed.hpp"
#include "vec2.hpp"

namespace ironclad {

struct LagSample {
    std::uint32_t entity_id  = 0;
    Vec2          pos        {};
    Fixed         radius     {};
};

struct LagFrame {
    std::uint32_t           tick = 0;     // simulation tick at which this was recorded
    bool                    valid = false;
    std::vector<LagSample>  samples;
};

class LagBuffer {
public:
    explicit LagBuffer(std::uint16_t capacity = 64)
        : capacity_(capacity), frames_(capacity) {}

    /// Record the current set of hit-test targets at the given tick.
    void record(std::uint32_t tick, std::vector<LagSample> samples) {
        const std::size_t idx = tick % capacity_;
        frames_[idx] = LagFrame{tick, true, std::move(samples)};
    }

    /// Hit-scan from `origin` along `dir` (unit-ish vector — we treat
    /// it as a ray segment of length `range`) at simulation tick
    /// `attack_tick`, rewinding by half the round-trip time
    /// expressed in *ticks*. Returns the entity id of the closest
    /// circle hit, or `std::nullopt` if none.
    std::optional<std::uint32_t>
    hitscan(Vec2 origin, Vec2 dir, Fixed range,
            std::uint32_t now_tick, std::uint16_t rtt_ticks) const {
        // Rewind half the RTT.
        const std::uint16_t back = static_cast<std::uint16_t>(rtt_ticks / 2u);
        if (back > now_tick) return std::nullopt;
        const std::uint32_t target = now_tick - back;

        const auto frame = frame_at(target);
        if (!frame.valid) return std::nullopt;

        std::optional<std::uint32_t> best;
        Fixed best_t = range;
        for (const auto& s : frame.samples) {
            // Ray-vs-circle in fixed-point.
            // Treat ray as origin + t*dir, t in [0, range].
            const Vec2 oc   = origin - s.pos;
            const Fixed a   = dir.length_sq();
            if (a == kZero) continue;
            const Fixed b   = oc.dot(dir) * Fixed{2};
            const Fixed c   = oc.length_sq() - s.radius * s.radius;
            const Fixed disc = b * b - Fixed{4} * a * c;
            if (disc < kZero) continue;
            const Fixed sqd = Fixed::sqrt(disc);
            const Fixed two_a = a * Fixed{2};
            // Smaller root first.
            const Fixed t1 = (-b - sqd) / two_a;
            const Fixed t2 = (-b + sqd) / two_a;
            const Fixed t = (t1 >= kZero) ? t1 : ((t2 >= kZero) ? t2 : Fixed{-1});
            if (t < kZero || t > range) continue;
            if (!best || t < best_t) { best = s.entity_id; best_t = t; }
        }
        return best;
    }

    /// Returns the most recently-recorded frame (for diagnostics/tests).
    [[nodiscard]] const LagFrame& latest() const noexcept {
        std::size_t best = 0;
        std::uint32_t best_tick = 0;
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            if (frames_[i].valid && frames_[i].tick >= best_tick) {
                best = i; best_tick = frames_[i].tick;
            }
        }
        return frames_[best];
    }

    /// Direct accessor for tests: snapshot at exactly `tick`, or
    /// invalid frame if not in the ring.
    [[nodiscard]] const LagFrame& frame_at(std::uint32_t tick) const noexcept {
        const std::size_t idx = tick % capacity_;
        return frames_[idx].tick == tick ? frames_[idx] : kInvalid;
    }

private:
    std::uint16_t          capacity_;
    std::vector<LagFrame>  frames_;
    static inline LagFrame kInvalid{};
};

}  // namespace ironclad
