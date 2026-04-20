// SPDX-License-Identifier: MIT
//
// Tests for the Replay Studio model + re-simulator.
//
// We exercise:
//   * v2 round-trip including rollback / flags / pred_diff.
//   * Forward-compat: a v1-magic file still parses cleanly with
//     zero rollback lane.
//   * `Replayer::world_at(frame)` produces byte-identical worlds
//     to a fresh deterministic simulation, on every frame and
//     in any scrub order (forward, backward, jump).
//   * `Replayer::validate_hash_chain` returns kNoDivergence on
//     intact replays and the correct frame on a tampered hash.
//   * `ReplayModel::stats()` counts events / max / avg / histogram
//     correctly, and `ReplayStats::rollback_event_count` excludes
//     the boring zero-rollback majority of frames.
//   * Event navigation (`next_event_index` / `prev_event_index`).
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <ironclad/byteio.hpp>
#include <ironclad/components.hpp>
#include <ironclad/ecs.hpp>
#include <ironclad/recorder.hpp>
#include <ironclad/replay.hpp>

using namespace ironclad;

// ----- Tiny deterministic simulation matching arena_demo --------------
// We deliberately re-implement the demo's init+step here rather than
// pulling in the apps/ source — that keeps the test entirely inside
// the library and immune to demo-side refactors.
namespace {

void test_init(World& w, Rng& rng, const SessionConfig& cfg) {
    w.register_component<Transform>();
    w.register_component<Velocity>();
    w.register_component<Player>();
    w.register_component<Projectile>();
    w.register_component<Hitbox>();
    for (std::uint8_t p = 0; p < cfg.num_players; ++p) {
        Entity e = w.create();
        w.add(e, Transform{Vec2{Fixed{p * 4}, Fixed{0}}, Fixed{}});
        w.add(e, Velocity{});
        w.add(e, Player{p, 0, 0, 1, Fixed{100}, 0});
        w.add(e, Hitbox{Fixed::from_ratio(1, 2)});
    }
    (void)rng;
}

void test_step(World& w, const PlayerInput* inputs,
               std::uint8_t num_players, Rng& rng) {
    for (std::uint8_t p = 0; p < num_players; ++p) {
        Entity self = kInvalidEntity;
        w.each<Player>([&](Entity e, Player& pl) {
            if (pl.id == p) self = e;
        });
        if (self == kInvalidEntity) continue;
        auto* tr  = w.get<Transform>(self);
        auto* vel = w.get<Velocity>(self);
        if (!tr || !vel) continue;
        vel->v.x = inputs[p].move_x_fx() * Fixed::from_ratio(1, 4);
        vel->v.y = inputs[p].move_y_fx() * Fixed::from_ratio(1, 4);
        tr->pos += vel->v;
    }
    (void)rng.next_u64();
}

PlayerInput synth_input(std::uint32_t frame, std::uint8_t player) {
    PlayerInput in;
    int phase = static_cast<int>(frame) + player * 32;
    in.move_x = static_cast<std::int8_t>(((phase / 17) & 1) ? 64 : -64);
    in.move_y = static_cast<std::int8_t>(((phase / 23) & 1) ? 64 : -64);
    if ((frame % 30u) == player) in.buttons |= PlayerInput::kAttack;
    return in;
}

// Build a synthetic recording of `frames` ticks with `players`
// players. Every Nth frame we inject a non-zero rollback / desync /
// pred_diff value via record_v2 so the model has interesting events
// to surface.
std::vector<std::uint8_t>
make_recording(std::uint32_t frames, std::uint8_t players,
               std::uint64_t seed = 0xCAFE'F00D'BEEF'BABEULL,
               bool inject_events = true) {
    World w(64);
    Rng rng(seed);
    SessionConfig cfg;
    cfg.num_players    = players;
    cfg.world_capacity = 64;
    cfg.seed           = seed;
    cfg.tick_hz        = 60;
    test_init(w, rng, cfg);

    Recorder rec;
    {
        ByteWriter snap;
        w.serialize(snap);
        snap.write_u64(rng.state());
        rec.begin(60, players, seed, 64, snap.view());
    }

    std::uint64_t final_hash = 0;
    std::vector<PlayerInput> inputs(players);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint8_t p = 0; p < players; ++p) inputs[p] = synth_input(f, p);
        test_step(w, inputs.data(), players, rng);
        ByteWriter h;
        w.serialize(h);
        h.write_u64(rng.state());
        final_hash = ironclad::hash64(h.view().data(), h.view().size());
        std::uint8_t rollback  = 0;
        std::uint8_t flags     = 0;
        std::uint8_t pred_diff = 0;
        if (inject_events) {
            if (f != 0 && (f % 50u) == 0) rollback = static_cast<std::uint8_t>(2 + (f / 50u) % 6);
            if (f == 137)                 flags    = ReplayRecord::kFlagDesync;
            if ((f % 73u) == 0)           pred_diff = static_cast<std::uint8_t>(1u << (f % players));
        }
        rec.record_v2(f, inputs, final_hash, rollback, flags, pred_diff);
    }
    return rec.finish(final_hash);
}

}  // namespace

TEST_CASE("v2 round-trip: rollback / flags / pred_diff survive parse") {
    auto bytes = make_recording(60, 3);
    auto m = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m.has_value());
    CHECK(m->header().version     == 4);
    CHECK(m->header().num_players == 3);
    CHECK(m->record_count()       == 60u);

    // Frame 50 was injected with rollback=4 (2 + (50/50)%6 = 2+1=3? recompute)
    // Use explicit re-derivation rather than trusting the magic number.
    std::uint8_t expected_rollback = static_cast<std::uint8_t>(2 + (50u / 50u) % 6u);
    CHECK(m->records()[50].rollback == expected_rollback);

    // pred_diff at frame 0 should be set (0 % 73 == 0).
    CHECK(m->records()[0].pred_diff != 0u);
}

TEST_CASE("v1 magic file still parses (rollback lane = 0)") {
    // Build a v1 file by hand from the layout described in
    // recorder.hpp. We then verify that the parser accepts it and
    // leaves the v2/v3-only fields zeroed.
    constexpr std::uint8_t  nplayers = 2;
    constexpr std::uint32_t frames   = 4;
    ByteWriter w;
    w.write_bytes("IRCL_REPLAY1", 12);
    w.write_u16(1);            // version
    w.write_u16(60);           // tick_hz
    w.write_u8(nplayers);
    w.write_u8(0);             // reserved
    w.write_u64(0xCAFEBABEULL);
    w.write_u32(64);           // world_capacity
    w.write_u32(0);            // init_size (no payload)
    for (std::uint32_t f = 0; f < frames; ++f) {
        w.write_u32(f);
        for (std::uint8_t p = 0; p < nplayers; ++p) {
            PlayerInput in{};
            in.move_x = static_cast<std::int8_t>(f * (p + 1));
            pack(w, in);
        }
        w.write_u64(0xAA00 + f);
    }
    w.write_bytes("ENDR", 4);
    w.write_u32(frames);
    w.write_u64(0xBEEFCAFE'12345678ULL);

    auto m = ReplayModel::load({w.view().data(), w.view().size()});
    REQUIRE(m.has_value());
    CHECK(m->header().version  == 1);
    CHECK(m->record_count()    == frames);
    CHECK(m->lag_events().empty());
    for (const auto& rec : m->records()) {
        CHECK(rec.rollback  == 0u);
        CHECK(rec.flags     == 0u);
        CHECK(rec.pred_diff == 0u);
    }
}

TEST_CASE("v4 predicted-input matrix round-trips") {
    constexpr std::uint8_t  nplayers = 3;
    Recorder rec;
    rec.begin(60, nplayers, 0xCAFE'F00DULL, 64, {});
    std::vector<PlayerInput> applied(nplayers);
    // Build a non-trivial predicted matrix per frame.
    for (std::uint32_t f = 0; f < 5; ++f) {
        for (std::uint8_t p = 0; p < nplayers; ++p) {
            applied[p].move_x = static_cast<std::int8_t>(p * 10 + f);
        }
        std::vector<PlayerInput> predicted(
            static_cast<std::size_t>(nplayers) * nplayers);
        for (std::uint8_t obs = 0; obs < nplayers; ++obs) {
            for (std::uint8_t tgt = 0; tgt < nplayers; ++tgt) {
                std::size_t idx =
                    static_cast<std::size_t>(obs) * nplayers + tgt;
                PlayerInput& cell = predicted[idx];
                if (obs == tgt) cell = applied[tgt];
                else cell.move_x = static_cast<std::int8_t>(99 - tgt);
            }
        }
        rec.record_v3(f, applied, 0xC0DE + f, 0, 0, 0,
                      {predicted.data(), predicted.size()});
    }
    auto bytes = rec.finish(0);
    auto m = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m.has_value());
    CHECK(m->header().version == 4);
    CHECK(m->record_count() == 5u);
    for (std::uint32_t f = 0; f < 5; ++f) {
        const auto& rec_in = m->records()[f];
        REQUIRE(rec_in.predicted.size() ==
                static_cast<std::size_t>(nplayers) * nplayers);
        for (std::uint8_t obs = 0; obs < nplayers; ++obs) {
            for (std::uint8_t tgt = 0; tgt < nplayers; ++tgt) {
                std::size_t idx =
                    static_cast<std::size_t>(obs) * nplayers + tgt;
                const auto& cell = rec_in.predicted[idx];
                if (obs == tgt) {
                    CHECK(cell.move_x == static_cast<std::int8_t>(tgt * 10 + f));
                } else {
                    CHECK(cell.move_x == static_cast<std::int8_t>(99 - tgt));
                }
            }
        }
    }
}

TEST_CASE("v2 file (no lag block) still parses") {
    constexpr std::uint8_t  nplayers = 2;
    constexpr std::uint32_t frames   = 4;
    ByteWriter w;
    w.write_bytes("IRCL_REPLAY2", 12);
    w.write_u16(2);
    w.write_u16(60);
    w.write_u8(nplayers);
    w.write_u8(0);
    w.write_u64(0xC0DECAFE);
    w.write_u32(64);
    w.write_u32(0);
    for (std::uint32_t f = 0; f < frames; ++f) {
        w.write_u32(f);
        for (std::uint8_t p = 0; p < nplayers; ++p) pack(w, PlayerInput{});
        w.write_u64(0xBB00 + f);
        w.write_u8(0); w.write_u8(0); w.write_u8(0);   // event lane
    }
    w.write_bytes("ENDR", 4);
    w.write_u32(frames);
    w.write_u64(0xFEEDC0DE);
    auto m = ReplayModel::load({w.view().data(), w.view().size()});
    REQUIRE(m.has_value());
    CHECK(m->header().version == 2);
    CHECK(m->lag_events().empty());
}

TEST_CASE("v3 lag events round-trip and are discoverable via nearest_lag_event") {
    Recorder rec;
    rec.begin(60, 2, 0xC0DEULL, 64, {});
    std::vector<PlayerInput> empty(2);
    for (std::uint32_t f = 0; f < 100; ++f) {
        rec.record_v2(f, empty, 0xCC00 + f, 0, 0, 0);
    }
    LagEvent e1; e1.frame = 25; e1.attacker_id = 0; e1.target_id = 1;
                 e1.rewound_ticks = 9; e1.range = Fixed{8};
                 e1.origin = Vec2{Fixed{1}, Fixed{1}}; e1.dir = Vec2{Fixed{1}, Fixed{0}};
    LagEvent e2 = e1; e2.frame = 70; e2.target_id = 0xFF;
    rec.record_lag_event(e1);
    rec.record_lag_event(e2);
    auto bytes = rec.finish(0xFEEDFEED);

    auto m = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m.has_value());
    CHECK(m->header().version == 4);
    REQUIRE(m->lag_events().size() == 2);
    CHECK(m->lag_events()[0].frame == 25);
    CHECK(m->lag_events()[0].target_id == 1);
    CHECK(m->lag_events()[0].rewound_ticks == 9);
    CHECK(m->lag_events()[1].target_id == 0xFFu);

    // nearest_lag_event lookup.
    const auto* nearest = m->nearest_lag_event(28, 5);
    REQUIRE(nearest != nullptr);
    CHECK(nearest->frame == 25);
    CHECK(m->nearest_lag_event(40, 3) == nullptr);
}

TEST_CASE("Replayer::world_at reproduces every recorded frame byte-identically") {
    constexpr std::uint32_t kFrames = 200;
    auto bytes = make_recording(kFrames, 4, 0x1234'ABCDULL, /*inject_events=*/true);
    auto m = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m.has_value());

    Replayer rp(*m, test_init, test_step, /*ckpt=*/30);

    // Build the ground-truth world frame-by-frame using a fresh
    // simulation (not Replayer) and compare snapshots at every
    // frame in non-monotonic order.
    SessionConfig cfg;
    cfg.num_players = 4;
    cfg.world_capacity = 64;
    cfg.seed = 0x1234'ABCDULL;
    cfg.tick_hz = 60;
    World gt(64); Rng grng(cfg.seed);
    test_init(gt, grng, cfg);
    std::vector<std::vector<std::uint8_t>> ground_truth(kFrames + 1);
    {
        ByteWriter w; gt.serialize(w);
        ground_truth[0].assign(w.view().begin(), w.view().end());
    }
    std::vector<PlayerInput> inputs(4);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint8_t p = 0; p < 4; ++p) inputs[p] = synth_input(f, p);
        test_step(gt, inputs.data(), 4, grng);
        ByteWriter w; gt.serialize(w);
        ground_truth[f + 1].assign(w.view().begin(), w.view().end());
    }

    // Scrub forwards.
    for (std::uint32_t f = 0; f <= kFrames; ++f) {
        const World& w = rp.world_at(f);
        ByteWriter ww; w.serialize(ww);
        std::vector<std::uint8_t> got(ww.view().begin(), ww.view().end());
        REQUIRE(got == ground_truth[f]);
    }
    // Scrub backwards.
    for (std::uint32_t f = kFrames + 1; f-- > 0;) {
        const World& w = rp.world_at(f);
        ByteWriter ww; w.serialize(ww);
        std::vector<std::uint8_t> got(ww.view().begin(), ww.view().end());
        REQUIRE(got == ground_truth[f]);
    }
    // Random jumps.
    std::uint32_t order[] = {73, 12, 199, 50, 0, 100, 137, 12, 200, 1};
    for (auto f : order) {
        const World& w = rp.world_at(f);
        ByteWriter ww; w.serialize(ww);
        std::vector<std::uint8_t> got(ww.view().begin(), ww.view().end());
        REQUIRE(got == ground_truth[f]);
    }
}

TEST_CASE("validate_hash_chain finds tampered records") {
    auto bytes = make_recording(60, 2, 0xFEEDFACEULL);
    {
        auto m = ReplayModel::load({bytes.data(), bytes.size()});
        REQUIRE(m.has_value());
        Replayer rp(*m, test_init, test_step, 30);
        CHECK(rp.validate_hash_chain() == Replayer::kNoDivergence);
    }
    // Now tamper: corrupt the hash of frame 23 directly in the
    // parsed model's records via re-loading after byte mutation.
    // Header walk: same as v1-compat test.
    constexpr std::size_t hdr_fixed = 12 + 2 + 2 + 1 + 1 + 8 + 4 + 4;
    std::uint32_t init_size = static_cast<std::uint32_t>(bytes[30]) |
                              (static_cast<std::uint32_t>(bytes[31]) << 8) |
                              (static_cast<std::uint32_t>(bytes[32]) << 16) |
                              (static_cast<std::uint32_t>(bytes[33]) << 24);
    std::size_t records_start = hdr_fixed + init_size;
    constexpr std::uint8_t nplayers = 2;
    // v4 record: 4 frame + 4*N inputs + 8 hash + 3 event-lane
    //          + 4*N*N predicted-input matrix.
    constexpr std::size_t v4_record_size =
        4 + 4u * nplayers + 8 + 3 + 4u * nplayers * nplayers;
    std::size_t target_record = records_start + 23u * v4_record_size;
    // Hash starts at offset (4 + 4*nplayers) inside the record.
    std::size_t hash_offset = target_record + 4 + 4u * nplayers;
    bytes[hash_offset] ^= 0xFFu;     // flip a byte

    auto m2 = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m2.has_value());
    Replayer rp2(*m2, test_init, test_step, 30);
    CHECK(rp2.validate_hash_chain() == 23u);
}

TEST_CASE("stats and event lanes match the recorded data") {
    auto bytes = make_recording(200, 4, 0xC0DEC0DEULL);
    auto m = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m.has_value());

    const auto& s = m->stats();
    CHECK(s.frame_count == 200u);

    // Re-derive expected events from the same injection rule used
    // in `make_recording` so any change there is caught here.
    std::uint64_t exp_total       = 0;
    std::uint8_t  exp_max         = 0;
    std::uint32_t exp_count       = 0;
    std::uint32_t exp_desync      = 0;
    for (std::uint32_t f = 0; f < 200; ++f) {
        std::uint8_t r = (f != 0 && (f % 50u) == 0)
            ? static_cast<std::uint8_t>(2 + (f / 50u) % 6) : 0;
        if (r > 0) { ++exp_count; exp_total += r; if (r > exp_max) exp_max = r; }
        if (f == 137) ++exp_desync;
    }
    CHECK(s.total_rollback_frames == exp_total);
    CHECK(s.max_rollback_frames   == exp_max);
    CHECK(s.rollback_event_count  == exp_count);
    CHECK(s.desync_event_count    == exp_desync);
    CHECK(s.avg_rollback_frames > 0.0);
    CHECK(s.avg_rollback_frames < static_cast<double>(exp_max));
}

TEST_CASE("event navigation: prev / next finds nearest rollback") {
    auto bytes = make_recording(200, 4);
    auto m = ReplayModel::load({bytes.data(), bytes.size()});
    REQUIRE(m.has_value());
    REQUIRE_FALSE(m->events().empty());

    // Next event from frame 0 should be the first rollback frame
    // (50, given our injection rule), with desync at 137 also in
    // the lane.
    auto i = m->next_event_index(0);
    REQUIRE(i < m->events().size());
    CHECK(m->events()[i].frame >= 50u);

    // Prev from end should be the last rollback / desync event.
    auto j = m->prev_event_index(199);
    REQUIRE(j < m->events().size());
    CHECK(m->events()[j].frame <= 199u);

    // Out-of-range with no wrap returns size().
    CHECK(m->next_event_index(10'000u) == m->events().size());
}
