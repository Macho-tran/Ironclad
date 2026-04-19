// SPDX-License-Identifier: MIT
//
// Bit-level adaptive arithmetic coder, byte-tree model.
//
// Architecture:
//   * Encoder/Decoder share a `Model` that holds a small binary
//     decision tree. The tree has 256 internal nodes, one per byte
//     prefix length (1..8). Each node tracks count(0) and count(1)
//     among the bits seen with that prefix. Leaves are byte values.
//   * After each bit is encoded/decoded, the corresponding node's
//     counter is incremented. Counters cap at 4096 then halve.
//   * A 257th "symbol" (256) is treated as EOF. For simplicity it
//     is stored as a 9-bit code in the tree (the first bit decides
//     "literal byte" vs "EOF"). Encoding `EOF` writes a single
//     escape bit; encoding a real byte writes the escape bit + 8
//     value bits.
//
// The bit-level coder uses the textbook Witten/Neal/Cleary E1/E2/E3
// renormalization, which has no carry-propagation pitfalls.
#include <ironclad/range_coder.hpp>

#include <array>
#include <cassert>
#include <cstring>

namespace ironclad {

namespace {

constexpr std::uint32_t kFirstQtr = 0x4000'0000u;   // 1/4
constexpr std::uint32_t kHalf     = 0x8000'0000u;   // 1/2
constexpr std::uint32_t kThirdQtr = 0xC000'0000u;   // 3/4

// One binary decision: holds counts of 0s and 1s seen in this context.
struct BitCtx {
    std::uint32_t c0 = 1;
    std::uint32_t c1 = 1;

    [[nodiscard]] std::uint32_t total() const noexcept { return c0 + c1; }

    void update(int bit) {
        if (bit == 0) ++c0; else ++c1;
        if (total() >= 4096u) {
            c0 = (c0 + 1u) >> 1;
            c1 = (c1 + 1u) >> 1;
        }
    }
};

// Model: one BitCtx for the EOF-vs-literal escape, then a 256-node
// binary tree indexed by partial-byte prefix.
struct Model {
    BitCtx               escape;          // bit 0 = literal, bit 1 = EOF
    std::array<BitCtx, 256> tree{};       // tree[1] = root, tree[2..3] = level 2, ...

    static constexpr unsigned kEofSym = 256;

    // Returns the node index for a byte prefix of length `bits` whose
    // value (right-aligned, MSB-first) is `prefix`.
    static constexpr std::size_t node(unsigned bits, unsigned prefix) {
        // bits in [0..7]; prefix in [0, 1<<bits).
        return (1u << bits) + prefix;
    }
};

// ----- Bit-level arithmetic coder -----------------------------------------
class BitEncoder {
public:
    explicit BitEncoder(std::vector<std::uint8_t>& out) : out_(out) {}

    // Encode a single bit with probability of 0 = c0 / (c0 + c1).
    void put_bit(int bit, std::uint32_t c0, std::uint32_t total) {
        const std::uint64_t range = static_cast<std::uint64_t>(high_ - low_) + 1;
        const std::uint32_t mid   = low_ + static_cast<std::uint32_t>(
            (range * c0) / total - 1);
        if (bit == 0) high_ = mid;
        else          low_  = mid + 1;
        renormalize();
    }

    void finish() {
        // Flush enough bits so the decoded interval is unambiguous.
        ++pending_;
        if (low_ < kFirstQtr) emit_bit_with_pending(0);
        else                  emit_bit_with_pending(1);
        // Pad current bit-buffer byte with zeros and write it.
        if (bit_pos_ != 0) {
            out_.push_back(bit_buf_);
            bit_buf_ = 0;
            bit_pos_ = 0;
        }
    }

private:
    void renormalize() {
        while (true) {
            if (high_ < kHalf) {
                emit_bit_with_pending(0);
            } else if (low_ >= kHalf) {
                emit_bit_with_pending(1);
                low_  -= kHalf;
                high_ -= kHalf;
            } else if (low_ >= kFirstQtr && high_ < kThirdQtr) {
                ++pending_;
                low_  -= kFirstQtr;
                high_ -= kFirstQtr;
            } else {
                break;
            }
            low_  <<= 1;
            high_  = (high_ << 1) | 1u;
        }
    }

    void emit_bit_with_pending(int bit) {
        emit_bit(bit);
        const int neg = bit ^ 1;
        while (pending_ > 0) {
            emit_bit(neg);
            --pending_;
        }
    }

    void emit_bit(int bit) {
        bit_buf_ = static_cast<std::uint8_t>(
            bit_buf_ | (bit << (7 - bit_pos_)));
        if (++bit_pos_ == 8) {
            out_.push_back(bit_buf_);
            bit_buf_ = 0;
            bit_pos_ = 0;
        }
    }

    std::vector<std::uint8_t>& out_;
    std::uint32_t              low_     = 0;
    std::uint32_t              high_    = 0xFFFF'FFFFu;
    unsigned                   pending_ = 0;
    std::uint8_t               bit_buf_ = 0;
    int                        bit_pos_ = 0;
};

class BitDecoder {
public:
    BitDecoder(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {
        for (int i = 0; i < 32; ++i) {
            code_ = (code_ << 1) | static_cast<std::uint32_t>(read_bit());
        }
    }

    int decode_bit(std::uint32_t c0, std::uint32_t total) {
        const std::uint64_t range = static_cast<std::uint64_t>(high_ - low_) + 1;
        const std::uint32_t mid   = low_ + static_cast<std::uint32_t>(
            (range * c0) / total - 1);
        int bit;
        if (code_ <= mid) { bit = 0; high_ = mid; }
        else              { bit = 1; low_  = mid + 1; }
        renormalize();
        return bit;
    }

    [[nodiscard]] bool error() const noexcept { return error_; }

private:
    void renormalize() {
        while (true) {
            if (high_ < kHalf) {
                // shift in a 0 at the top — implicit
            } else if (low_ >= kHalf) {
                code_ -= kHalf; low_ -= kHalf; high_ -= kHalf;
            } else if (low_ >= kFirstQtr && high_ < kThirdQtr) {
                code_ -= kFirstQtr; low_ -= kFirstQtr; high_ -= kFirstQtr;
            } else {
                break;
            }
            low_  <<= 1;
            high_  = (high_ << 1) | 1u;
            code_  = (code_ << 1) | static_cast<std::uint32_t>(read_bit());
        }
    }

    int read_bit() {
        if (bit_pos_ == 0) {
            bit_buf_ = pos_ < size_ ? data_[pos_++] : std::uint8_t{0};
            bit_pos_ = 8;
        }
        int b = (bit_buf_ >> 7) & 1;
        bit_buf_ = static_cast<std::uint8_t>(bit_buf_ << 1);
        --bit_pos_;
        return b;
    }

    const std::uint8_t* data_;
    std::size_t         size_;
    std::size_t         pos_     = 0;
    std::uint32_t       low_     = 0;
    std::uint32_t       high_    = 0xFFFF'FFFFu;
    std::uint32_t       code_    = 0;
    std::uint8_t        bit_buf_ = 0;
    int                 bit_pos_ = 0;
    bool                error_   = false;
};

// ----- Symbol-level coding using the model -------------------------------
void put_symbol(BitEncoder& enc, Model& m, unsigned sym) {
    if (sym == Model::kEofSym) {
        // Escape bit = 1 means EOF.
        enc.put_bit(1, m.escape.c0, m.escape.total());
        m.escape.update(1);
        return;
    }
    // Escape bit = 0 means a literal byte follows.
    enc.put_bit(0, m.escape.c0, m.escape.total());
    m.escape.update(0);

    unsigned prefix = 0;
    for (int level = 0; level < 8; ++level) {
        const int bit = static_cast<int>((sym >> (7 - level)) & 1u);
        const std::size_t node = Model::node(static_cast<unsigned>(level), prefix);
        auto& ctx = m.tree[node];
        enc.put_bit(bit, ctx.c0, ctx.total());
        ctx.update(bit);
        prefix = (prefix << 1) | static_cast<unsigned>(bit);
    }
}

int get_symbol(BitDecoder& dec, Model& m) {
    const int esc = dec.decode_bit(m.escape.c0, m.escape.total());
    m.escape.update(esc);
    if (esc == 1) return -1;   // EOF

    unsigned prefix = 0;
    for (int level = 0; level < 8; ++level) {
        const std::size_t node = Model::node(static_cast<unsigned>(level), prefix);
        auto& ctx = m.tree[node];
        const int bit = dec.decode_bit(ctx.c0, ctx.total());
        ctx.update(bit);
        prefix = (prefix << 1) | static_cast<unsigned>(bit);
    }
    return static_cast<int>(prefix);
}

}  // namespace

std::size_t RangeCoder::encode(std::span<const std::uint8_t> in,
                               std::vector<std::uint8_t>&    out) {
    const std::size_t before = out.size();
    Model      m{};
    BitEncoder enc(out);
    for (auto b : in) put_symbol(enc, m, b);
    put_symbol(enc, m, Model::kEofSym);
    enc.finish();
    return out.size() - before;
}

bool RangeCoder::decode(std::span<const std::uint8_t> in,
                        std::vector<std::uint8_t>&    out) {
    Model      m{};
    BitDecoder dec(in.data(), in.size());
    while (true) {
        int sym = get_symbol(dec, m);
        if (dec.error()) return false;
        if (sym < 0) break;
        out.push_back(static_cast<std::uint8_t>(sym));
        if (out.size() > 32u * 1024u * 1024u) return false;  // sanity cap
    }
    return true;
}

}  // namespace ironclad
