// SPDX-License-Identifier: MIT
#include <ironclad/desync.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace ironclad {

DesyncDumper::DesyncDumper(std::string_view directory,
                           std::string_view role)
    : directory_(directory), role_(role) {
    std::error_code ec;
    (void)std::filesystem::create_directories(directory_, ec);
    // We swallow `ec`: dump() will surface failure as an empty path.
}

std::string DesyncDumper::dump(std::uint32_t frame,
                               std::span<const std::uint8_t> local_bytes,
                               std::uint64_t local_hash,
                               std::uint64_t remote_hash) {
    char filename[256];
    std::snprintf(filename, sizeof(filename),
                  "desync_%s_frame_%010u.bin", role_.c_str(), frame);
    std::filesystem::path bin_path = std::filesystem::path(directory_) / filename;
    {
        std::ofstream f(bin_path, std::ios::binary);
        if (!f) return {};
        f.write(reinterpret_cast<const char*>(local_bytes.data()),
                static_cast<std::streamsize>(local_bytes.size()));
    }
    char meta_name[256];
    std::snprintf(meta_name, sizeof(meta_name),
                  "desync_%s_frame_%010u.txt", role_.c_str(), frame);
    std::filesystem::path meta_path = std::filesystem::path(directory_) / meta_name;
    {
        std::ofstream f(meta_path);
        if (!f) return {};
        f << "role         : " << role_ << "\n";
        f << "frame        : " << frame << "\n";
        f << "local_hash   : 0x" << std::hex << local_hash  << std::dec << "\n";
        f << "remote_hash  : 0x" << std::hex << remote_hash << std::dec << "\n";
        f << "snapshot_len : " << local_bytes.size() << " bytes\n";
    }
    return bin_path.string();
}

}  // namespace ironclad
