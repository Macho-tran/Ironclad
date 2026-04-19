// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <cstdint>
#include <vector>

#include <ironclad/recorder.hpp>

using namespace ironclad;

TEST_CASE("recorder writes a parseable header + records + trailer") {
    Recorder rec;
    std::vector<std::uint8_t> initial(64);
    for (std::size_t i = 0; i < initial.size(); ++i)
        initial[i] = static_cast<std::uint8_t>(i);

    rec.begin(/*tick_hz=*/60, /*num_players=*/4,
              /*seed=*/0xDEADBEEFCAFEBABEULL,
              /*world_capacity=*/256,
              {initial.data(), initial.size()});

    for (std::uint32_t f = 0; f < 30; ++f) {
        std::vector<PlayerInput> inputs(4);
        for (std::uint8_t p = 0; p < 4; ++p) {
            inputs[p].buttons = static_cast<std::uint16_t>(f * 4u + p);
            inputs[p].move_x  = static_cast<std::int8_t>((p * 13 + f) % 127);
        }
        rec.record(f, {inputs.data(), inputs.size()}, 0xCC00 + f);
    }
    auto bytes = rec.finish(0xFEEDF00DCAFEBABEULL);

    ReplayHeader              hdr;
    std::vector<ReplayRecord> records;
    std::uint64_t             final_hash = 0;
    REQUIRE(parse_replay({bytes.data(), bytes.size()}, hdr, records, final_hash));
    CHECK(hdr.tick_hz       == 60);
    CHECK(hdr.num_players   == 4);
    CHECK(hdr.seed          == 0xDEADBEEFCAFEBABEULL);
    CHECK(hdr.world_capacity == 256u);
    CHECK(hdr.initial_snapshot == initial);
    REQUIRE(records.size() == 30);
    for (std::uint32_t f = 0; f < 30; ++f) {
        CHECK(records[f].frame == f);
        REQUIRE(records[f].inputs.size() == 4);
        for (std::uint8_t p = 0; p < 4; ++p) {
            CHECK(records[f].inputs[p].buttons ==
                  static_cast<std::uint16_t>(f * 4u + p));
        }
        CHECK(records[f].hash == 0xCC00 + f);
    }
    CHECK(final_hash == 0xFEEDF00DCAFEBABEULL);
}

TEST_CASE("recorder rejects mismatched player count silently (count stays 0)") {
    Recorder rec;
    rec.begin(60, 4, 0, 256, {});
    std::vector<PlayerInput> wrong(2);
    rec.record(0, {wrong.data(), wrong.size()}, 1234);
    CHECK(rec.frame_count() == 0u);   // refused
}

TEST_CASE("parse_replay rejects truncated input") {
    Recorder rec;
    rec.begin(60, 2, 0, 256, {});
    std::vector<PlayerInput> in(2);
    rec.record(0, {in.data(), in.size()}, 7);
    auto bytes = rec.finish(8);

    ReplayHeader              hdr;
    std::vector<ReplayRecord> records;
    std::uint64_t             final_hash = 0;
    bytes.resize(bytes.size() / 2);
    CHECK_FALSE(parse_replay({bytes.data(), bytes.size()}, hdr, records, final_hash));
}

TEST_CASE("parse_replay rejects bad magic") {
    Recorder rec;
    rec.begin(60, 2, 0, 256, {});
    std::vector<PlayerInput> in(2);
    rec.record(0, {in.data(), in.size()}, 1);
    auto bytes = rec.finish(2);
    bytes[0] = 'X';
    ReplayHeader              hdr;
    std::vector<ReplayRecord> records;
    std::uint64_t             final_hash = 0;
    CHECK_FALSE(parse_replay({bytes.data(), bytes.size()}, hdr, records, final_hash));
}
