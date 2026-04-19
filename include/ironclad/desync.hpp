// SPDX-License-Identifier: MIT
//
// Desync detector. The Session already records *that* a desync
// happened (via SessionStats::desync_detected). This module wraps
// the dump-to-disk side: when triggered, it writes both the local
// and remote snapshot bytes to files named by role + frame so the
// developer can binary-diff them.
//
// We deliberately keep the API minimal and synchronous. The .bin
// files are exactly the bytes returned by `Session::snapshot_for`
// for a given frame, so any external diff tool can compare them.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ironclad {

class DesyncDumper {
public:
    /// Construct with a directory + role tag (e.g. "host", "client").
    /// The directory is created if it does not exist.
    DesyncDumper(std::string_view directory, std::string_view role);

    /// Dump the local snapshot for `frame` plus a small text file
    /// listing the local + remote hash. Returns the local file path.
    std::string dump(std::uint32_t frame,
                     std::span<const std::uint8_t> local_bytes,
                     std::uint64_t local_hash,
                     std::uint64_t remote_hash);

private:
    std::string directory_;
    std::string role_;
};

}  // namespace ironclad
