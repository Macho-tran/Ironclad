// SPDX-License-Identifier: MIT
//
// Snapshot ring buffer used by the rollback Session.
//
// A snapshot is the entire serialized world state for one frame,
// plus the 64-bit hash of that state. The buffer is a circular
// array indexed by `frame % capacity`. The session never reads
// stale entries because it tracks `oldest_frame_` and asserts on
// any access outside the live window.
//
// The buffer also stores the per-frame hash so we can detect a
// desync without re-serializing: the host sends its hash, the
// client checks against the value already in its local ring.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ironclad {

struct Snapshot {
    std::uint32_t              frame = 0;
    std::uint64_t              hash  = 0;
    std::vector<std::uint8_t>  bytes;
};

class SnapshotRing {
public:
    explicit SnapshotRing(std::uint16_t capacity)
        : capacity_(capacity), entries_(capacity), valid_(capacity, false) {
        assert(capacity > 0 && (capacity & (capacity - 1u)) == 0u &&
               "capacity must be a power of two");
    }

    [[nodiscard]] std::uint16_t capacity() const noexcept { return capacity_; }

    /// Insert a snapshot for `frame`. Overwrites the slot for the
    /// previous occupant of that ring index.
    void store(std::uint32_t frame, std::uint64_t hash,
               std::vector<std::uint8_t> bytes) {
        const std::size_t idx = frame & static_cast<std::uint32_t>(capacity_ - 1);
        entries_[idx] = Snapshot{frame, hash, std::move(bytes)};
        valid_[idx]   = true;
    }

    /// Returns nullptr if the requested frame is not currently stored
    /// (either too old or never produced).
    [[nodiscard]] const Snapshot* get(std::uint32_t frame) const noexcept {
        const std::size_t idx = frame & static_cast<std::uint32_t>(capacity_ - 1);
        if (!valid_[idx])              return nullptr;
        if (entries_[idx].frame != frame) return nullptr;
        return &entries_[idx];
    }

    /// True if the given frame is currently in the ring.
    [[nodiscard]] bool contains(std::uint32_t frame) const noexcept {
        return get(frame) != nullptr;
    }

    /// Returns the oldest currently-stored frame, or `std::uint32_t(-1)`
    /// if the ring is empty.
    [[nodiscard]] std::uint32_t oldest_frame() const noexcept {
        std::uint32_t oldest = ~std::uint32_t{0};
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (valid_[i] && entries_[i].frame < oldest) {
                oldest = entries_[i].frame;
            }
        }
        return oldest;
    }

private:
    std::uint16_t                  capacity_;
    std::vector<Snapshot>          entries_;
    std::vector<std::uint8_t>      valid_;  // bool packed as bytes for stable layout
};

// Ring buffer of per-player inputs. Indexed by frame, with a fixed
// capacity. When a frame leaves the window it is forgotten — the
// session is responsible for not asking about it.
template <typename T>
class FrameRing {
public:
    explicit FrameRing(std::uint16_t capacity)
        : capacity_(capacity), entries_(capacity), valid_(capacity, false) {
        assert(capacity > 0 && (capacity & (capacity - 1u)) == 0u);
    }

    void store(std::uint32_t frame, T value) {
        const std::size_t idx = frame & static_cast<std::uint32_t>(capacity_ - 1);
        entries_[idx]      = value;
        frames_[idx_safe(idx)] = frame;
        valid_[idx]        = true;
    }

    [[nodiscard]] const T* get(std::uint32_t frame) const noexcept {
        const std::size_t idx = frame & static_cast<std::uint32_t>(capacity_ - 1);
        if (!valid_[idx])               return nullptr;
        if (frames_[idx] != frame)      return nullptr;
        return &entries_[idx];
    }

    [[nodiscard]] bool contains(std::uint32_t frame) const noexcept {
        return get(frame) != nullptr;
    }

    [[nodiscard]] std::uint16_t capacity() const noexcept { return capacity_; }

private:
    // Helper to silence sign-conversion warnings when computing `frames_`.
    static constexpr std::size_t idx_safe(std::size_t i) noexcept { return i; }

    std::uint16_t              capacity_;
    std::vector<T>             entries_;
    std::vector<std::uint32_t> frames_  = std::vector<std::uint32_t>(capacity_, 0);
    std::vector<std::uint8_t>  valid_;
};

}  // namespace ironclad
