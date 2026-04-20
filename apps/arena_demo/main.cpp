// SPDX-License-Identifier: MIT
#include "arena.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <ironclad/replay.hpp>

namespace {

// ---- Replay-info helper --------------------------------------------------
//
// Parses an .iclr file, prints a developer-friendly summary, and
// optionally re-simulates to confirm the recorded hash chain matches.
// This is the text companion to the SDL `arena_view --replay` mode;
// it makes the replay studio useful in CI and over SSH.

int print_replay_info(const char* path) {
    auto m = ironclad::ReplayModel::load_file(path);
    if (!m) {
        std::fprintf(stderr, "failed to load replay: %s\n", path);
        return 2;
    }
    const auto& h = m->header();
    const auto& s = m->stats();

    std::printf("== ironclad replay ==\n");
    std::printf("  file              : %s\n", path);
    std::printf("  format version    : v%u\n", static_cast<unsigned>(h.version));
    std::printf("  tick rate         : %u Hz\n", static_cast<unsigned>(h.tick_hz));
    std::printf("  players           : %u\n", static_cast<unsigned>(h.num_players));
    std::printf("  seed              : 0x%016lx\n",
                static_cast<unsigned long>(h.seed));
    std::printf("  world capacity    : %u entities\n", h.world_capacity);
    std::printf("  initial snapshot  : %lu bytes\n",
                static_cast<unsigned long>(h.initial_snapshot.size()));
    std::printf("  recorded frames   : %u\n", s.frame_count);
    std::printf("  duration          : %.2f s @ %u Hz\n",
                static_cast<double>(s.frame_count) /
                    static_cast<double>(std::max<std::uint16_t>(h.tick_hz, 1)),
                static_cast<unsigned>(h.tick_hz));
    std::printf("  rollback events   : %u (max %u frames, total %lu, avg %.2f)\n",
                s.rollback_event_count,
                static_cast<unsigned>(s.max_rollback_frames),
                static_cast<unsigned long>(s.total_rollback_frames),
                s.avg_rollback_frames);
    std::printf("  desync events     : %u\n", s.desync_event_count);
    std::printf("  rollback histogram:\n");
    static const char* labels[] = {
        "  =0f", "  =1f", "  <=2f", "  <=4f", "  <=8f",
        " <=16f", " <=32f", "  >32f"
    };
    for (std::size_t i = 0; i < s.rollback_histogram.size(); ++i) {
        std::printf("     %s : %u\n", labels[i], s.rollback_histogram[i]);
    }

    // Re-simulate and validate the hash chain — if anything in the
    // library has regressed since the recording, this catches it.
    ironclad::Replayer rp(*m, arena_demo::init_arena, arena_demo::step_arena);
    auto bad = rp.validate_hash_chain();
    if (bad == ironclad::Replayer::kNoDivergence) {
        std::printf("  hash chain        : OK (re-sim matches every recorded hash)\n");
    } else {
        std::printf("  hash chain        : DIVERGED at frame %u\n", bad);
        return 3;
    }

    return 0;
}

}  // namespace

namespace {

bool parse_u32(const char* s, std::uint32_t& out) {
    char* end = nullptr;
    auto v = std::strtoul(s, &end, 0);
    if (!end || *end != 0) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}
bool parse_u16(const char* s, std::uint16_t& out) {
    std::uint32_t v = 0;
    if (!parse_u32(s, v) || v > 0xFFFFu) return false;
    out = static_cast<std::uint16_t>(v);
    return true;
}
bool parse_u8(const char* s, std::uint8_t& out) {
    std::uint32_t v = 0;
    if (!parse_u32(s, v) || v > 0xFFu) return false;
    out = static_cast<std::uint8_t>(v);
    return true;
}
bool parse_u64(const char* s, std::uint64_t& out) {
    char* end = nullptr;
    auto v = std::strtoull(s, &end, 0);
    if (!end || *end != 0) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

void usage() {
    std::fprintf(stderr,
        "arena_demo - headless ironclad rollback netcode demo\n"
        "Usage:\n"
        "  arena_demo [--frames N] [--players P]\n"
        "             [--rtt-ms M] [--jitter-ms J]\n"
        "             [--loss-pct L] [--reorder-pct R]\n"
        "             [--seed 0xC0FFEE] [--record PATH] [--quiet]\n"
        "  arena_demo --replay-info PATH\n"
        "      print a summary of an .iclr recording (and validate the\n"
        "      hash chain by deterministically re-simulating it).\n"
        "Defaults: frames=3600 players=4 rtt-ms=0 jitter-ms=0 loss-pct=0 reorder-pct=0\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Special-case the replay-info verb — it bypasses the rest of the
    // option parsing and just inspects an .iclr file.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--replay-info") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--replay-info requires a path\n");
                return 2;
            }
            return print_replay_info(argv[i + 1]);
        }
    }

    arena_demo::Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (++i >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                std::exit(2);
            }
            return argv[i];
        };
        if      (a == "--frames")      { if (!parse_u32(need("--frames"),     o.frames))      std::exit(2); }
        else if (a == "--players")     { if (!parse_u8 (need("--players"),    o.num_players)) std::exit(2); }
        else if (a == "--rtt-ms")      { if (!parse_u16(need("--rtt-ms"),     o.rtt_ms))      std::exit(2); }
        else if (a == "--jitter-ms")   { if (!parse_u16(need("--jitter-ms"),  o.jitter_ms))   std::exit(2); }
        else if (a == "--loss-pct")    { if (!parse_u8 (need("--loss-pct"),   o.loss_pct))    std::exit(2); }
        else if (a == "--reorder-pct") { if (!parse_u8 (need("--reorder-pct"),o.reorder_pct)) std::exit(2); }
        else if (a == "--seed")        { if (!parse_u64(need("--seed"),       o.seed))        std::exit(2); }
        else if (a == "--record")      { o.record_path = need("--record"); }
        else if (a == "--quiet")       { o.quiet = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }

    auto r = arena_demo::run(o);
    std::printf(
        "\n=== arena_demo summary ===\n"
        "  total ticks            : %lu\n"
        "  total rollback frames  : %lu\n"
        "  avg rollback per tick  : %.3f frames\n"
        "  bytes sent / client    : %lu\n"
        "  bandwidth / client     : %.2f KB/s (simulated wall-time)\n"
        "  desync detected        : %s\n",
        static_cast<unsigned long>(r.total_ticks),
        static_cast<unsigned long>(r.total_rollback_frames),
        r.avg_rollback_frames,
        static_cast<unsigned long>(r.bytes_sent_per_client),
        r.bandwidth_kbps_per_client,
        r.desync_detected ? "YES" : "no");
    return r.ok ? 0 : 1;
}
