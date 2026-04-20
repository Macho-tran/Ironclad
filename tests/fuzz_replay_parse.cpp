// SPDX-License-Identifier: MIT
//
// libFuzzer harness for parse_replay / ReplayModel::load.
//
// Goal: feed arbitrary byte sequences into the replay parser and
// verify it never crashes, never reads OOB, and never trips
// UB-Sanitizer. We don't check return value — partial parses are
// expected on most random inputs.
#include <cstddef>
#include <cstdint>

#include <ironclad/replay.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
    auto m = ironclad::ReplayModel::load({data, size});
    if (m) {
        // If the file parsed, also exercise a few derived APIs so
        // they get coverage too.
        (void)m->record_count();
        (void)m->stats();
        (void)m->events();
        (void)m->lag_events();
        (void)m->nearest_lag_event(0, 8);
        (void)m->nearest_lag_event(m->record_count() / 2, 4);
        if (!m->records().empty()) {
            (void)m->record_index_for_frame(m->records().front().frame);
            (void)m->record_index_for_frame(m->records().back().frame);
        }
    }
    return 0;
}
