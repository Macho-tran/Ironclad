// SPDX-License-Identifier: MIT
#include "doctest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <ironclad/desync.hpp>

using namespace ironclad;

static std::filesystem::path tmp_subdir(const char* tag) {
    auto p = std::filesystem::temp_directory_path() / "ironclad_test" / tag;
    std::filesystem::remove_all(p);
    return p;
}

TEST_CASE("DesyncDumper writes the snapshot bytes and metadata") {
    auto dir = tmp_subdir("desync_basic");
    DesyncDumper dumper(dir.string(), "host");
    std::vector<std::uint8_t> snap(128);
    for (std::size_t i = 0; i < snap.size(); ++i) snap[i] = static_cast<std::uint8_t>(i);

    std::string bin_path = dumper.dump(42, {snap.data(), snap.size()},
                                       0xAABBCCDDEEFF0011ULL,
                                       0x1122334455667788ULL);
    REQUIRE_FALSE(bin_path.empty());
    REQUIRE(std::filesystem::exists(bin_path));
    CHECK(std::filesystem::file_size(bin_path) == snap.size());

    // Verify the metadata sidecar.
    std::filesystem::path meta = bin_path;
    meta.replace_extension(".txt");
    REQUIRE(std::filesystem::exists(meta));

    std::string content;
    {
        std::ifstream f(meta);
        REQUIRE(f.good());
        std::stringstream ss;
        ss << f.rdbuf();
        content = ss.str();
    }
    CHECK(content.find("frame        : 42")           != std::string::npos);
    CHECK(content.find("role         : host")         != std::string::npos);
    CHECK(content.find("snapshot_len : 128 bytes")    != std::string::npos);

    // Read back the bin and verify equality.
    std::vector<char> raw;
    {
        std::ifstream bf(bin_path, std::ios::binary | std::ios::ate);
        REQUIRE(bf.good());
        const auto sz = bf.tellg();
        bf.seekg(0);
        raw.resize(static_cast<std::size_t>(sz));
        bf.read(raw.data(), sz);
    }
    std::vector<std::uint8_t> read(raw.begin(), raw.end());
    CHECK(read == snap);
}

TEST_CASE("DesyncDumper creates the target directory if missing") {
    auto dir = tmp_subdir("desync_mkdir") / "nested" / "deep";
    DesyncDumper dumper(dir.string(), "client");
    std::vector<std::uint8_t> snap{1, 2, 3, 4};
    std::string p = dumper.dump(7, {snap.data(), snap.size()}, 1, 2);
    CHECK_FALSE(p.empty());
    CHECK(std::filesystem::exists(p));
}
