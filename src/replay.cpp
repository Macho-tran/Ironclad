// SPDX-License-Identifier: MIT
#include <ironclad/replay.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

#include <ironclad/byteio.hpp>
#include <ironclad/hash.hpp>

namespace ironclad {

namespace {

constexpr std::array<std::uint8_t, 8> kHistogramBucketEnds = {
    0, 1, 2, 4, 8, 16, 32, 64,
};

std::size_t histogram_bucket(std::uint8_t rollback) noexcept {
    for (std::size_t i = 0; i < kHistogramBucketEnds.size() - 1; ++i) {
        if (rollback <= kHistogramBucketEnds[i]) return i;
    }
    return kHistogramBucketEnds.size() - 1;
}

void compute_stats_and_events(const std::vector<ReplayRecord>& records,
                              ReplayStats&                     stats,
                              std::vector<RollbackEvent>&      events) {
    stats = ReplayStats{};
    events.clear();
    stats.frame_count = static_cast<std::uint32_t>(records.size());
    for (const auto& r : records) {
        if (r.rollback > 0) {
            ++stats.rollback_event_count;
            stats.total_rollback_frames +=
                static_cast<std::uint64_t>(r.rollback);
            if (r.rollback > stats.max_rollback_frames) {
                stats.max_rollback_frames = r.rollback;
            }
        }
        if (r.flags & ReplayRecord::kFlagDesync) {
            ++stats.desync_event_count;
        }
        ++stats.rollback_histogram[histogram_bucket(r.rollback)];
        if (r.rollback > 0 || (r.flags & ReplayRecord::kFlagDesync)) {
            RollbackEvent ev;
            ev.frame    = r.frame;
            ev.distance = r.rollback;
            ev.desync   = (r.flags & ReplayRecord::kFlagDesync) != 0;
            events.push_back(ev);
        }
    }
    if (stats.frame_count > 0) {
        stats.avg_rollback_frames =
            static_cast<double>(stats.total_rollback_frames) /
            static_cast<double>(stats.frame_count);
    }
}

}  // namespace

// ----- ReplayModel -------------------------------------------------------

std::optional<ReplayModel>
ReplayModel::load(std::span<const std::uint8_t> bytes) {
    ReplayModel m;
    if (!parse_replay(bytes, m.hdr_, m.records_, m.lag_events_, m.final_hash_)) {
        return std::nullopt;
    }
    compute_stats_and_events(m.records_, m.stats_, m.events_);
    return m;
}

const LagEvent* ReplayModel::nearest_lag_event(std::uint32_t frame,
                                               std::uint32_t window) const noexcept {
    const LagEvent* best = nullptr;
    std::uint32_t   best_d = window + 1;
    for (const auto& ev : lag_events_) {
        const std::uint32_t d = ev.frame > frame
            ? ev.frame - frame
            : frame - ev.frame;
        if (d < best_d) { best = &ev; best_d = d; }
    }
    return best;
}

std::optional<ReplayModel>
ReplayModel::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) return std::nullopt;
    const auto sz = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f.good()) return std::nullopt;
    return load({buf.data(), buf.size()});
}

std::size_t
ReplayModel::next_event_index(std::uint32_t frame, bool wrap) const noexcept {
    for (std::size_t i = 0; i < events_.size(); ++i) {
        if (events_[i].frame >= frame) return i;
    }
    if (wrap && !events_.empty()) return 0;
    return events_.size();
}

std::size_t
ReplayModel::prev_event_index(std::uint32_t frame, bool wrap) const noexcept {
    std::size_t found = events_.size();
    for (std::size_t i = 0; i < events_.size(); ++i) {
        if (events_[i].frame <= frame) found = i;
        else break;
    }
    if (found == events_.size() && wrap && !events_.empty()) {
        return events_.size() - 1;
    }
    return found;
}

std::size_t
ReplayModel::record_index_for_frame(std::uint32_t frame) const noexcept {
    // Records are stored in monotonic frame order (Recorder appends
    // sequentially), but we use binary search to be robust.
    auto it = std::lower_bound(records_.begin(), records_.end(), frame,
        [](const ReplayRecord& r, std::uint32_t f) { return r.frame < f; });
    if (it == records_.end() || it->frame != frame) return records_.size();
    return static_cast<std::size_t>(std::distance(records_.begin(), it));
}

// ----- Replayer ----------------------------------------------------------

Replayer::Replayer(const ReplayModel& model,
                   WorldInit          init,
                   SimStep            step,
                   std::uint16_t      checkpoint_interval)
    : model_(model),
      init_(std::move(init)),
      step_(std::move(step)),
      checkpoint_interval_(checkpoint_interval == 0 ? 60 : checkpoint_interval),
      world_(model.header().world_capacity),
      rng_(model.header().seed) {
    // Run the user's init function — it must register the same
    // component types in the same order as the recording session,
    // otherwise the recorded hashes won't reproduce.
    SessionConfig cfg;
    cfg.num_players    = model.header().num_players;
    cfg.tick_hz        = model.header().tick_hz;
    cfg.seed           = model.header().seed;
    cfg.world_capacity = model.header().world_capacity;
    init_(world_, rng_, cfg);
    current_frame_ = 0;
    capture_checkpoint(0);
}

void Replayer::capture_checkpoint(std::uint32_t frame) {
    Checkpoint c;
    c.frame = frame;
    ByteWriter w;
    world_.serialize(w);
    c.bytes     = w.take();
    c.rng_state = rng_.state();
    // Keep checkpoints sorted by frame, replace if exact match.
    auto it = std::lower_bound(checkpoints_.begin(), checkpoints_.end(), frame,
        [](const Checkpoint& cp, std::uint32_t f) { return cp.frame < f; });
    if (it != checkpoints_.end() && it->frame == frame) {
        *it = std::move(c);
    } else {
        checkpoints_.insert(it, std::move(c));
    }
}

void Replayer::load_checkpoint(std::uint32_t frame) {
    auto it = std::lower_bound(checkpoints_.begin(), checkpoints_.end(), frame,
        [](const Checkpoint& cp, std::uint32_t f) { return cp.frame < f; });
    if (it == checkpoints_.end() || it->frame > frame) {
        if (it == checkpoints_.begin()) return;
        --it;
    }
    ByteReader r(it->bytes.data(), it->bytes.size());
    [[maybe_unused]] bool ok = world_.deserialize(r);
    rng_.set_state(it->rng_state);
    current_frame_ = it->frame;
}

void Replayer::rebuild_to(std::uint32_t frame) {
    if (frame > model_.record_count()) frame = model_.record_count();
    if (frame == current_frame_) return;

    if (frame < current_frame_) {
        load_checkpoint(frame);
    }

    while (current_frame_ < frame) {
        // Apply the canonical inputs for `current_frame_` and step
        // forward by one tick.
        const std::size_t idx = static_cast<std::size_t>(current_frame_);
        if (idx >= model_.records().size()) break;
        const auto& rec = model_.records()[idx];
        step_(world_, rec.inputs.data(),
              static_cast<std::uint8_t>(rec.inputs.size()), rng_);
        ++current_frame_;
        if ((current_frame_ % checkpoint_interval_) == 0) {
            capture_checkpoint(current_frame_);
        }
    }
}

const World& Replayer::world_at(std::uint32_t frame) {
    rebuild_to(frame);
    return world_;
}

std::uint32_t Replayer::validate_hash_chain() {
    // Re-simulate from frame 0 to the end, comparing each tick's
    // post-step hash against the recorded hash. Returns the first
    // frame where the chain breaks, or kNoDivergence on success.
    load_checkpoint(0);
    for (std::size_t i = 0; i < model_.records().size(); ++i) {
        const auto& rec = model_.records()[i];
        step_(world_, rec.inputs.data(),
              static_cast<std::uint8_t>(rec.inputs.size()), rng_);
        ++current_frame_;
        // Compute hash the same way Session does: serialize world
        // bytes followed by the RNG state.
        ByteWriter w;
        world_.serialize(w);
        w.write_u64(rng_.state());
        const std::uint64_t h = hash64(w.view().data(), w.view().size());
        if (h != rec.hash) {
            return rec.frame;
        }
        if ((current_frame_ % checkpoint_interval_) == 0) {
            capture_checkpoint(current_frame_);
        }
    }
    return kNoDivergence;
}

}  // namespace ironclad
