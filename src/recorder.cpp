// SPDX-License-Identifier: MIT
#include <ironclad/recorder.hpp>

#include <cstring>

#include <ironclad/byteio.hpp>

namespace ironclad {

namespace {
constexpr char        kMagic[]       = "IRCL_REPLAY1";   // 12 bytes incl. NUL
constexpr std::size_t kMagicSize     = 12;
constexpr char        kTrailerMagic[] = "ENDR";
constexpr std::uint16_t kReplayVersion = 1;
}  // namespace

void Recorder::begin(std::uint16_t tick_hz,
                     std::uint8_t  num_players,
                     std::uint64_t seed,
                     std::uint32_t world_capacity,
                     std::span<const std::uint8_t> initial_snapshot) {
    out_.clear();
    num_players_ = num_players;
    frames_      = 0;

    ByteWriter w;
    w.write_bytes(kMagic, kMagicSize);
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

void Recorder::record(std::uint32_t frame,
                      std::span<const PlayerInput> per_player_inputs,
                      std::uint64_t hash) {
    if (per_player_inputs.size() != num_players_) {
        // We could throw; for the test+demo path we just refuse.
        return;
    }
    ByteWriter w;
    w.write_u32(frame);
    for (auto in : per_player_inputs) pack(w, in);
    w.write_u64(hash);
    out_.insert(out_.end(), w.view().begin(), w.view().end());
    ++frames_;
}

std::vector<std::uint8_t> Recorder::finish(std::uint64_t final_hash) {
    ByteWriter w;
    w.write_bytes(kTrailerMagic, 4);
    w.write_u32(frames_);
    w.write_u64(final_hash);
    out_.insert(out_.end(), w.view().begin(), w.view().end());
    return std::move(out_);
}

bool parse_replay(std::span<const std::uint8_t> bytes,
                  ReplayHeader&                 hdr,
                  std::vector<ReplayRecord>&    records,
                  std::uint64_t&                final_hash) {
    ByteReader r(bytes.data(), bytes.size());
    char magic[kMagicSize];
    r.read_bytes(magic, kMagicSize);
    if (r.error() || std::memcmp(magic, kMagic, kMagicSize) != 0) return false;
    hdr.version       = r.read_u16();
    if (hdr.version != kReplayVersion) return false;
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
    while (true) {
        // Peek for trailer magic.
        if (r.remaining() < 4) return false;
        if (bytes[r.pos()]     == 'E' &&
            bytes[r.pos() + 1] == 'N' &&
            bytes[r.pos() + 2] == 'D' &&
            bytes[r.pos() + 3] == 'R') break;
        ReplayRecord rec;
        rec.frame = r.read_u32();
        rec.inputs.resize(hdr.num_players);
        for (std::uint8_t p = 0; p < hdr.num_players; ++p) {
            rec.inputs[p] = unpack_input(r);
        }
        rec.hash = r.read_u64();
        if (r.error()) return false;
        records.push_back(std::move(rec));
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
