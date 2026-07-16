#pragma once
//
// A NASDAQ TotalView-ITCH 5.0 parser for the raw file format: a stream of
// [2-byte big-endian length][message] records, message type in the first byte.
// Zero-copy: the Reader hands out views into its buffer; decode_* functions pull
// big-endian fields on demand. Only the fields the book builder needs are decoded;
// every other message type is skipped by length (the length prefix makes the whole
// stream skippable without knowing every layout).
//
// Spec: Nasdaq TotalView-ITCH 5.0, rev 4.1. Field offsets below are byte offsets
// FROM THE MESSAGE TYPE BYTE (offset 0 = the type char itself).
//
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace itch {

inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | p[3];
}
inline std::uint64_t be48(const std::uint8_t* p) {  // ITCH timestamps: nanos since midnight
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

// A view of one message: `p` points at the type byte, `len` covers the whole message
// (excluding the 2-byte length prefix). Valid until the next Reader::next() call.
struct MsgView {
    const std::uint8_t* p;
    std::size_t len;
    char type() const { return static_cast<char>(p[0]); }
};

// Expected message lengths (type byte included) for the messages we decode — used to
// reject truncated/corrupt records before touching their fields.
inline std::size_t expected_len(char type) {
    switch (type) {
        case 'S': return 12;  // system event
        case 'R': return 39;  // stock directory
        case 'H': return 25;  // stock trading action
        case 'A': return 36;  // add order
        case 'F': return 40;  // add order with MPID
        case 'E': return 31;  // order executed
        case 'C': return 36;  // order executed with price
        case 'X': return 23;  // order cancel (partial)
        case 'D': return 19;  // order delete
        case 'U': return 35;  // order replace
        case 'P': return 44;  // trade (hidden / non-displayed)
        case 'Q': return 40;  // cross trade
        default: return 0;    // unknown to us: any length accepted, skipped
    }
}

// ---- typed decoders (caller must have checked len via expected_len) -------------
struct SystemEvent {
    std::uint64_t ts;
    char code;  // O S Q M E C — 'Q' = start of market hours, 'M' = end
};
inline SystemEvent decode_system(const std::uint8_t* p) { return {be48(p + 5), static_cast<char>(p[11])}; }

struct StockDirectory {
    std::uint16_t locate;
    char stock[9];  // 8 chars, space-padded on the wire; NUL-terminated here
};
inline StockDirectory decode_directory(const std::uint8_t* p) {
    StockDirectory d{};
    d.locate = be16(p + 1);
    std::memcpy(d.stock, p + 11, 8);
    d.stock[8] = '\0';
    return d;
}

struct AddOrder {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    char side;  // 'B' or 'S'
    std::uint32_t shares;
    std::uint32_t price4;  // price in 1/10000 dollars
};
inline AddOrder decode_add(const std::uint8_t* p) {  // 'A' and 'F' share the layout prefix
    return {be16(p + 1), be48(p + 5), be64(p + 11), static_cast<char>(p[19]), be32(p + 20), be32(p + 32)};
}

struct OrderExec {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    std::uint32_t shares;
};
inline OrderExec decode_exec(const std::uint8_t* p) {  // 'E'; 'C' shares the prefix
    return {be16(p + 1), be48(p + 5), be64(p + 11), be32(p + 19)};
}
// 'C' extras: printable flag + execution price (book effect is identical to 'E').
inline bool exec_c_printable(const std::uint8_t* p) { return p[31] == 'Y'; }

struct OrderCancel {  // 'X': partial cancel, order keeps its queue position
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    std::uint32_t shares;  // cancelled amount
};
inline OrderCancel decode_cancel(const std::uint8_t* p) {
    return {be16(p + 1), be48(p + 5), be64(p + 11), be32(p + 19)};
}

struct OrderDelete {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
};
inline OrderDelete decode_delete(const std::uint8_t* p) { return {be16(p + 1), be48(p + 5), be64(p + 11)}; }

struct OrderReplace {  // 'U': new ref, new price/size, queue priority LOST (per spec)
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t orig_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price4;
};
inline OrderReplace decode_replace(const std::uint8_t* p) {
    return {be16(p + 1), be48(p + 5), be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31)};
}

struct HiddenTrade {  // 'P': execution against non-displayed liquidity; no book effect
    std::uint16_t locate;
    std::uint64_t ts;
    char side;
    std::uint32_t shares;
    std::uint32_t price4;
};
inline HiddenTrade decode_hidden_trade(const std::uint8_t* p) {
    return {be16(p + 1), be48(p + 5), static_cast<char>(p[19]), be32(p + 20), be32(p + 32)};
}

// ---- streaming reader ------------------------------------------------------------
// Reads the length-prefixed stream from a FILE* (a file, or a pipe such as
// `gunzip -c day.gz |`). Buffered and allocation-stable after construction.
class Reader {
public:
    explicit Reader(std::FILE* f, std::size_t buf_size = 1u << 20)
        : f_(f), buf_(buf_size), fill_(0), pos_(0) {}

    // Advance to the next message. Returns false on clean EOF; sets `truncated()` if
    // the stream ended mid-record (a partial download — everything before is valid).
    bool next(MsgView& out) {
        if (!ensure(2)) return false;
        const std::size_t len = be16(&buf_[pos_]);
        if (len == 0) { truncated_ = true; return false; }  // corrupt length
        if (!ensure(2 + len)) { truncated_ = true; return false; }
        out.p = &buf_[pos_ + 2];
        out.len = len;
        pos_ += 2 + len;
        ++count_;
        return true;
    }

    std::uint64_t count() const { return count_; }
    bool truncated() const { return truncated_; }

private:
    // Make at least `need` bytes available at pos_ (compacting + refilling).
    bool ensure(std::size_t need) {
        if (fill_ - pos_ >= need) return true;
        if (pos_ > 0) {  // compact the tail to the front
            std::memmove(buf_.data(), buf_.data() + pos_, fill_ - pos_);
            fill_ -= pos_;
            pos_ = 0;
        }
        while (fill_ < need) {
            const std::size_t got = std::fread(buf_.data() + fill_, 1, buf_.size() - fill_, f_);
            if (got == 0) return false;  // EOF or error
            fill_ += got;
        }
        return true;
    }

    std::FILE* f_;
    std::vector<std::uint8_t> buf_;  // contiguous, size-stable byte buffer
    std::size_t fill_;
    std::size_t pos_;
    std::uint64_t count_ = 0;
    bool truncated_ = false;
};

}  // namespace itch
