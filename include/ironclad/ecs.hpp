// SPDX-License-Identifier: MIT
//
// Tiny structure-of-arrays Entity-Component-System used by the
// deterministic simulation.
//
// Why this and not EnTT?
//   * EnTT's sparse-set iteration order can change depending on the
//     history of insertions/removals. That's fine for traditional
//     gameplay, but it makes byte-deterministic snapshots painful.
//   * Our needs are tiny: a few component types, fixed entity cap.
//   * A dense column store with a parallel "alive" bitmap serializes
//     to a byte-identical buffer regardless of compiler/STL version.
//
// Constraints on component types T:
//   * T must be standard-layout and trivially-copyable.
//   * The component author must provide free functions in T's
//     namespace (or in `ironclad::`):
//         void pack(ByteWriter&, const T&);
//         void unpack(ByteReader&, T&);
//     which produce a *fixed*-size byte representation. We deliberately
//     do NOT memcpy raw struct memory because of compiler padding; we
//     want the snapshot bytes to be portable across toolchains.
//   * The serialized size MUST be constant per component type.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "byteio.hpp"

namespace ironclad {

using Entity = std::uint32_t;
inline constexpr Entity kInvalidEntity = ~Entity{0};

namespace detail {

/// Small bitmap helper. Stored as a packed `std::vector<uint8_t>`
/// so that it serializes via memcpy.
class Bitmap {
public:
    void resize(std::size_t bits) {
        bits_  = bits;
        bytes_.assign((bits + 7) / 8, 0);
    }
    std::size_t  bit_count()  const noexcept { return bits_; }
    std::size_t  byte_count() const noexcept { return bytes_.size(); }
    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    std::uint8_t*       data()       noexcept { return bytes_.data(); }

    bool test(std::size_t i) const noexcept {
        const auto shift = static_cast<unsigned>(i & 7u);
        const auto byte  = static_cast<unsigned>(bytes_[i >> 3]);
        return ((byte >> shift) & 1u) != 0u;
    }
    void set(std::size_t i) noexcept {
        bytes_[i >> 3] = static_cast<std::uint8_t>(
            bytes_[i >> 3] | (1u << (i & 7u)));
    }
    void clear(std::size_t i) noexcept {
        bytes_[i >> 3] = static_cast<std::uint8_t>(
            bytes_[i >> 3] & ~(1u << (i & 7u)));
    }
    void clear_all() noexcept {
        std::fill(bytes_.begin(), bytes_.end(), std::uint8_t{0});
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t               bits_ = 0;
};

/// Type-erased base for a single component column.
struct IColumn {
    virtual ~IColumn() = default;
    virtual void clear() = 0;
    virtual void resize(std::uint32_t cap) = 0;
    virtual void destroy(Entity e) = 0;
    virtual std::uint32_t serialized_size_of_one() const = 0;
    virtual void          serialize(ByteWriter&) const = 0;
    virtual void          deserialize(ByteReader&, std::uint32_t cap) = 0;
};

template <typename T>
struct Column : IColumn {
    static_assert(std::is_standard_layout_v<T>,
                  "components must be standard-layout");
    static_assert(std::is_trivially_copyable_v<T>,
                  "components must be trivially copyable");

    Bitmap            has;
    std::vector<T>    data;

    void clear() override {
        has.clear_all();
        // We deliberately leave `data` contents unchanged for slots
        // whose has-bit is 0; they are unreachable until added back.
    }
    void resize(std::uint32_t cap) override {
        has.resize(cap);
        data.assign(cap, T{});
    }
    void destroy(Entity e) override {
        if (e < data.size()) has.clear(e);
    }
    std::uint32_t serialized_size_of_one() const override {
        // Probe by writing one zeroed instance and measuring. Done once
        // and cached on first call.
        if (cached_size_ == 0u) {
            ByteWriter w;
            T tmp{};
            pack(w, tmp);
            cached_size_ = static_cast<std::uint32_t>(w.size());
        }
        return cached_size_;
    }
    void serialize(ByteWriter& w) const override {
        // bitmap then packed components for slots with has-bit set.
        w.write_bytes(has.data(), has.byte_count());
        for (std::size_t i = 0; i < data.size(); ++i) {
            if (has.test(i)) {
                pack(w, data[i]);
            }
        }
    }
    void deserialize(ByteReader& r, std::uint32_t cap) override {
        resize(cap);
        r.read_bytes(has.data(), has.byte_count());
        for (std::size_t i = 0; i < data.size(); ++i) {
            if (has.test(i)) {
                unpack(r, data[i]);
            }
        }
    }

private:
    mutable std::uint32_t cached_size_ = 0;
};

/// Type id helper. Each call to `type_id<T>()` returns a stable
/// integer for the duration of the program. We use it only as a
/// debug check (registration order is what determines column id).
template <typename T>
inline std::size_t type_id() noexcept {
    static const int marker = 0;
    return reinterpret_cast<std::size_t>(&marker);
}

}  // namespace detail

/// World owns entity bookkeeping and an ordered list of component
/// columns. Component columns are registered up-front (e.g. at
/// session construction) and never reordered, which is what gives us
/// stable column ids across snapshots.
class World {
public:
    static constexpr std::uint32_t kSnapshotMagic   = 0x4C435249u;  // 'IRCL'
    static constexpr std::uint16_t kSnapshotVersion = 1;

    explicit World(std::uint32_t capacity = 256)
        : capacity_(capacity), next_id_(0) {
        alive_.resize(capacity_);
        free_list_.reserve(16);
    }

    // ----- Entity API ----------------------------------------------------
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t alive_count() const noexcept { return alive_count_; }

    bool is_alive(Entity e) const noexcept {
        return e < capacity_ && alive_.test(e);
    }

    Entity create() {
        Entity e;
        if (!free_list_.empty()) {
            e = free_list_.back();
            free_list_.pop_back();
        } else {
            assert(next_id_ < capacity_ && "World capacity exceeded");
            e = next_id_++;
        }
        alive_.set(e);
        ++alive_count_;
        return e;
    }

    void destroy(Entity e) {
        if (!is_alive(e)) return;
        for (auto& col : columns_) col->destroy(e);
        alive_.clear(e);
        --alive_count_;
        free_list_.push_back(e);
    }

    void clear_entities() {
        // Resets to a fresh state, re-using existing columns. Useful
        // for snapshot loading: we allocate the columns once.
        alive_.clear_all();
        for (auto& col : columns_) col->clear();
        next_id_     = 0;
        alive_count_ = 0;
        free_list_.clear();
    }

    // ----- Component registration ---------------------------------------
    /// Register component type T. Must be called once per component
    /// type, in the same order on every peer, before any tick runs.
    /// Returns the column id, used internally for snapshot order.
    template <typename T>
    std::size_t register_component() {
        auto col = std::make_unique<detail::Column<T>>();
        col->resize(capacity_);
        const std::size_t id = columns_.size();
        type_index_[detail::type_id<T>()] = id;
        columns_.push_back(std::move(col));
        return id;
    }

    template <typename T>
    [[nodiscard]] bool has_component(Entity e) const noexcept {
        if (!is_alive(e)) return false;
        const auto* col = column<T>();
        return col->has.test(e);
    }

    template <typename T>
    void add(Entity e, T value = {}) {
        assert(is_alive(e));
        auto* col = column<T>();
        col->data[e] = value;
        col->has.set(e);
    }

    template <typename T>
    void remove(Entity e) {
        if (!is_alive(e)) return;
        column<T>()->has.clear(e);
    }

    template <typename T>
    [[nodiscard]] T* get(Entity e) {
        auto* col = column<T>();
        return col->has.test(e) ? &col->data[e] : nullptr;
    }
    template <typename T>
    [[nodiscard]] const T* get(Entity e) const {
        const auto* col = column<T>();
        return col->has.test(e) ? &col->data[e] : nullptr;
    }

    /// Iterate every alive entity that has component T, in ascending
    /// entity order (deterministic).
    template <typename T, typename Fn>
    void each(Fn fn) {
        auto* col = column<T>();
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (alive_.test(i) && col->has.test(i)) {
                fn(static_cast<Entity>(i), col->data[i]);
            }
        }
    }
    template <typename T, typename Fn>
    void each(Fn fn) const {
        const auto* col = column<T>();
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (alive_.test(i) && col->has.test(i)) {
                fn(static_cast<Entity>(i), col->data[i]);
            }
        }
    }

    // ----- Serialization ------------------------------------------------
    /// Serialize the entire world into `w`. Output is compact and
    /// byte-deterministic.
    void serialize(ByteWriter& w) const {
        w.write_u32(kSnapshotMagic);
        w.write_u16(kSnapshotVersion);
        w.write_u32(capacity_);
        w.write_u32(next_id_);
        w.write_u32(alive_count_);
        // alive bitmap
        w.write_u32(static_cast<std::uint32_t>(alive_.byte_count()));
        w.write_bytes(alive_.data(), alive_.byte_count());
        // columns
        w.write_u32(static_cast<std::uint32_t>(columns_.size()));
        for (const auto& col : columns_) {
            col->serialize(w);
        }
    }

    /// Replace the entire world state from `r`. Columns must already
    /// be registered in the same order as the producer.
    [[nodiscard]] bool deserialize(ByteReader& r) {
        const auto magic   = r.read_u32();
        const auto version = r.read_u16();
        const auto cap     = r.read_u32();
        const auto next    = r.read_u32();
        const auto alive   = r.read_u32();
        const auto bm_sz   = r.read_u32();
        if (r.error()) return false;
        if (magic   != kSnapshotMagic)   return false;
        if (version != kSnapshotVersion) return false;
        if (cap     != capacity_)        return false;
        if (bm_sz   != alive_.byte_count()) return false;
        next_id_     = next;
        alive_count_ = alive;
        r.read_bytes(alive_.data(), alive_.byte_count());
        const auto ncols = r.read_u32();
        if (ncols != columns_.size()) return false;
        for (auto& col : columns_) {
            col->deserialize(r, capacity_);
        }
        free_list_.clear();  // free list is *not* serialized; rebuild on demand
        // Rebuild free list by scanning gaps below `next_id_`.
        for (std::uint32_t i = 0; i < next_id_; ++i) {
            if (!alive_.test(i)) free_list_.push_back(i);
        }
        return !r.error();
    }

private:
    template <typename T>
    detail::Column<T>* column() {
        const std::size_t idx = type_index_.lookup(detail::type_id<T>());
        return static_cast<detail::Column<T>*>(columns_[idx].get());
    }
    template <typename T>
    const detail::Column<T>* column() const {
        const std::size_t idx = type_index_.lookup(detail::type_id<T>());
        return static_cast<const detail::Column<T>*>(columns_[idx].get());
    }

    std::uint32_t                                capacity_;
    std::uint32_t                                next_id_;
    std::uint32_t                                alive_count_ = 0;
    detail::Bitmap                               alive_;
    std::vector<Entity>                          free_list_;
    std::vector<std::unique_ptr<detail::IColumn>> columns_;
    // Maps detail::type_id<T>() -> column index in `columns_`.
    // Implemented as a small flat map to avoid std::unordered_map's
    // hash randomization (we never iterate it, but better safe than
    // sorry for snapshot-equality reasoning).
    struct TypeIndexEntry { std::size_t key; std::size_t value; };
    struct TypeIndex {
        std::vector<TypeIndexEntry> entries;
        // Looks up `k`; asserts (debug) on miss. Component types must
        // be registered before any per-type lookup.
        std::size_t lookup(std::size_t k) const noexcept {
            for (const auto& e : entries) if (e.key == k) return e.value;
            assert(false && "component not registered");
            return 0;
        }
        std::size_t& operator[](std::size_t k) {
            for (auto& e : entries) if (e.key == k) return e.value;
            entries.push_back({k, 0});
            return entries.back().value;
        }
    };
    TypeIndex type_index_;
};

}  // namespace ironclad
