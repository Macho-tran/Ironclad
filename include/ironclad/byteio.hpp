// SPDX-License-Identifier: MIT
//
// Tiny little-endian byte stream readers and writers used for snapshot
// serialization, packet serialization, and replay file I/O.
//
// Determinism contract:
//   * Every multi-byte integer is written little-endian regardless of
//     host endianness.
//   * No floats, ever. The simulation has no float values to write;
//     the view layer serializes nothing.
//   * Out-of-bounds reads return zero/false rather than crashing,
//     and the reader records an `error()` flag that callers can
//     check after a complete decode pass.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace ironclad {

class ByteWriter {
public:
    void reserve(std::size_t n) { buf_.reserve(n); }

    void write_u8(std::uint8_t v) { buf_.push_back(v); }

    void write_u16(std::uint16_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v       & 0xFFu));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    }

    void write_u32(std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i)
            buf_.push_back(static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu));
    }

    void write_u64(std::uint64_t v) {
        for (unsigned i = 0; i < 8; ++i)
            buf_.push_back(static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu));
    }

    void write_i64(std::int64_t v) {
        write_u64(static_cast<std::uint64_t>(v));
    }

    void write_bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + n);
    }

    std::span<const std::uint8_t> view() const { return {buf_.data(), buf_.size()}; }
    std::vector<std::uint8_t>&    take()       { return buf_; }
    std::size_t                   size() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size), pos_(0), error_(false) {}

    explicit ByteReader(std::span<const std::uint8_t> s) noexcept
        : data_(s.data()), size_(s.size()), pos_(0), error_(false) {}

    [[nodiscard]] bool        error()    const noexcept { return error_; }
    [[nodiscard]] std::size_t pos()      const noexcept { return pos_; }
    [[nodiscard]] std::size_t size()     const noexcept { return size_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - pos_; }
    [[nodiscard]] bool        eof()      const noexcept { return pos_ >= size_; }

    std::uint8_t read_u8() noexcept {
        if (pos_ + 1 > size_) { error_ = true; return 0; }
        return data_[pos_++];
    }

    std::uint16_t read_u16() noexcept {
        if (pos_ + 2 > size_) { error_ = true; pos_ = size_; return 0; }
        std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                          (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return v;
    }

    std::uint32_t read_u32() noexcept {
        if (pos_ + 4 > size_) { error_ = true; pos_ = size_; return 0; }
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8u * i);
        pos_ += 4;
        return v;
    }

    std::uint64_t read_u64() noexcept {
        if (pos_ + 8 > size_) { error_ = true; pos_ = size_; return 0; }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8u * i);
        pos_ += 8;
        return v;
    }

    std::int64_t read_i64() noexcept {
        return static_cast<std::int64_t>(read_u64());
    }

    void read_bytes(void* dst, std::size_t n) noexcept {
        if (n == 0) return;     // memcpy(nullptr, ..., 0) is UB per C
        if (pos_ + n > size_) {
            error_ = true; pos_ = size_;
            if (dst) std::memset(dst, 0, n);
            return;
        }
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
    }

private:
    const std::uint8_t* data_;
    std::size_t         size_;
    std::size_t         pos_;
    bool                error_;
};

}  // namespace ironclad
