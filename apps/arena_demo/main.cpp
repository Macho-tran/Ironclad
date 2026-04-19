// SPDX-License-Identifier: MIT
#include "arena.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
        "Defaults: frames=3600 players=4 rtt-ms=0 jitter-ms=0 loss-pct=0 reorder-pct=0\n");
}

}  // namespace

int main(int argc, char** argv) {
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
