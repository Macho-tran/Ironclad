// SPDX-License-Identifier: MIT
#include <ironclad/recorder.hpp>

#include <cstring>

#include <ironclad/byteio.hpp>

namespace ironclad {

namespace {
constexpr char        kMagicV1[]       = "IRCL_REPLAY1";   // 12 bytes incl. NUL
constexpr char        kMagicV2[]       = "IRCL_REPLAY2";   // 12 bytes incl. NUL
constexpr char        kMagicV3[]       = "IRCL_REPLAY3";   // 12 bytes incl. NUL
constexpr std::size_t kMagicSize       = 12;
constexpr char        kTrailerMagic[]  = "ENDR";
constexpr char        kLagBlockMagic[] = "LAGE";           // before lag-event block
constexpr std::uint16_t kReplayVersion = 3;
}  // namespace

void Recorder::begin(std::uint16_t tick_hz,
                     std::uint8_t  num_players,
                     std::uint64_t seed,
                     std::uint32_t world_capacity,
                     std::span<const std::uint8_t> initial_snapshot) {
    out_.clear();
    num_players_ = num_players;
    frames_      = 0;

    lag_events_.clear();

    ByteWriter w;
    w.write_bytes(kMagicV3, kMagicSize);
    w.write_u16(kReplayVersion);
    w.write_u16(tick_hz);
    w.write_u8(num_players);
    w.write_u8(0);              // reserved
    w.write_u64(seed);
    w.write_u32(world_capacity);
    w.write_u32(static_cast<std::uint32_t>(initial_snapshot.size()));
    w.write_bytes(initial_snapshot.data(), initial_snapshot.size());
    out_ = std::move(w.take());
}

void Recorder::record_lag_event(const LagEvent& ev) {
    lag_events_.push_back(ev);
}

void Recorder::record(std::uint32_t frame,
                      std::span<const PlayerInput> per_player_inputs,
                      std::uint64_t hash) {
    record_v2(frame, per_player_inputs, hash, 0, 0, 0);
}

void Recorder::record_v2(std::uint32_t frame,
                         std::span<const PlayerInput> per_player_inputs,
                         std::uint64_t hash,
                         std::uint8_t  rollback,
                         std::uint8_t  flags,
                         std::uint8_t  pred_diff) {
    if (per_player_inputs.size() != num_players_) {
        // We could throw; for the test+demo path we just refuse.
        return;
    }
    ByteWriter w;
    w.write_u32(frame);
    for (auto in : per_player_inputs) pack(w, in);
    w.write_u64(hash);
    w.write_u8(rollback);
    w.write_u8(flags);
    w.write_u8(pred_diff);
    out_.insert(out_.end(), w.view().begin(), w.view().end());
    ++frames_;
}

std::vector<std::uint8_t> Recorder::finish(std::uint64_t final_hash) {
    ByteWriter w;
    // Lag-event block (v3+).
    w.write_bytes(kLagBlockMagic, 4);
    w.write_u32(static_cast<std::uint32_t>(lag_events_.size()));
    for (const auto& ev : lag_events_) {
        w.write_u32(ev.frame);
        w.write_u8(ev.attacker_id);
        w.write_u8(ev.target_id);
        w.write_u16(ev.rewound_ticks);
        w.write_i64(ev.origin.x.raw());
        w.write_i64(ev.origin.y.raw());
        w.write_i64(ev.dir.x.raw());
        w.write_i64(ev.dir.y.raw());
        w.write_i64(ev.range.raw());
    }
    // Trailer.
    w.write_bytes(kTrailerMagic, 4);
    w.write_u32(frames_);
    w.write_u64(final_hash);
    out_.insert(out_.end(), w.view().begin(), w.view().end());
    return std::move(out_);
}

bool parse_replay(std::span<const std::uint8_t> bytes,
                  ReplayHeader&                 hdr,
                  std::vector<ReplayRecord>&    records,
                  std::vector<LagEvent>&        lag_events,
                  std::uint64_t&                final_hash) {
    ByteReader r(bytes.data(), bytes.size());
    char magic[kMagicSize];
    r.read_bytes(magic, kMagicSize);
    if (r.error()) return false;
    const bool is_v3 = std::memcmp(magic, kMagicV3, kMagicSize) == 0;
    const bool is_v2 = std::memcmp(magic, kMagicV2, kMagicSize) == 0;
    const bool is_v1 = std::memcmp(magic, kMagicV1, kMagicSize) == 0;
    if (!is_v1 && !is_v2 && !is_v3) return false;

    hdr.version = r.read_u16();
    if ((is_v3 && hdr.version != 3) ||
        (is_v2 && hdr.version != 2) ||
        (is_v1 && hdr.version != 1)) {
        return false;
    }
    hdr.tick_hz       = r.read_u16();
    hdr.num_players   = r.read_u8();
    [[maybe_unused]] auto reserved = r.read_u8();
    hdr.seed          = r.read_u64();
    hdr.world_capacity = r.read_u32();
    auto init_size     = r.read_u32();
    if (r.error()) return false;
    hdr.initial_snapshot.resize(init_size);
    r.read_bytes(hdr.initial_snapshot.data(), init_size);
    if (r.error()) return false;

    records.clear();
    lag_events.clear();
    const bool has_event_lane = is_v2 || is_v3;
    while (true) {
        // Peek for end-of-records sentinel: either "ENDR" (v1/v2)
        // or "LAGE" (v3 lag block) or "ENDR" (v3 with no lag block).
        if (r.remaining() < 4) return false;
        const auto p0 = bytes[r.pos()];
        const auto p1 = bytes[r.pos() + 1];
        const auto p2 = bytes[r.pos() + 2];
        const auto p3 = bytes[r.pos() + 3];
        if ((p0 == 'E' && p1 == 'N' && p2 == 'D' && p3 == 'R') ||
            (p0 == 'L' && p1 == 'A' && p2 == 'G' && p3 == 'E')) {
            break;
        }
        ReplayRecord rec;
        rec.frame = r.read_u32();
        rec.inputs.resize(hdr.num_players);
        for (std::uint8_t p = 0; p < hdr.num_players; ++p) {
            rec.inputs[p] = unpack_input(r);
        }
        rec.hash = r.read_u64();
        if (has_event_lane) {
            rec.rollback  = r.read_u8();
            rec.flags     = r.read_u8();
            rec.pred_diff = r.read_u8();
        }
        if (r.error()) return false;
        records.push_back(std::move(rec));
    }

    // Optional lag-event block (v3+).
    if (is_v3 && r.remaining() >= 4 &&
        bytes[r.pos()]     == 'L' && bytes[r.pos() + 1] == 'A' &&
        bytes[r.pos() + 2] == 'G' && bytes[r.pos() + 3] == 'E') {
        char lblock[4];
        r.read_bytes(lblock, 4);
        const std::uint32_t n = r.read_u32();
        if (r.error()) return false;
        lag_events.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            LagEvent ev;
            ev.frame         = r.read_u32();
            ev.attacker_id   = r.read_u8();
            ev.target_id     = r.read_u8();
            ev.rewound_ticks = r.read_u16();
            ev.origin.x = Fixed::from_raw(r.read_i64());
            ev.origin.y = Fixed::from_raw(r.read_i64());
            ev.dir.x    = Fixed::from_raw(r.read_i64());
            ev.dir.y    = Fixed::from_raw(r.read_i64());
            ev.range    = Fixed::from_raw(r.read_i64());
            if (r.error()) return false;
            lag_events.push_back(ev);
        }
    }

    char trailer[4];
    r.read_bytes(trailer, 4);
    if (r.error() || std::memcmp(trailer, kTrailerMagic, 4) != 0) return false;
    auto frame_count_in_trailer = r.read_u32();
    final_hash = r.read_u64();
    if (r.error()) return false;
    if (frame_count_in_trailer != records.size()) return false;
    return true;
}

}  // namespace ironclad
