#pragma once
//
// A two-level occupancy bitmap over price ticks: O(1) set/clear and near-O(1)
// "next occupied tick >= i" / "prev occupied tick <= i". This is what keeps the
// best-bid / best-ask pointer moves off the O(ticks) linear scan a naive ladder
// would need when a far-away best level empties.
//
// Level 0 (`words_`): one bit per tick. Level 1 (`summary_`): one bit per level-0
// word, set iff that word is non-zero. A find touches at most ~ticks/4096 summary
// words, i.e. a handful for any realistic tick count.
//
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

class Occupancy {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    explicit Occupancy(std::size_t n_bits)
        : nbits_(n_bits),
          words_((n_bits + 63) / 64, 0),
          summary_((((n_bits + 63) / 64) + 63) / 64, 0) {}

    bool test(std::size_t i) const { return (words_[i >> 6] >> (i & 63)) & 1u; }

    void set(std::size_t i) {
        const std::size_t w = i >> 6;
        words_[w] |= bit(i & 63);
        summary_[w >> 6] |= bit(w & 63);
    }

    void clear(std::size_t i) {
        const std::size_t w = i >> 6;
        words_[w] &= ~bit(i & 63);
        if (words_[w] == 0) summary_[w >> 6] &= ~bit(w & 63);
    }

    // Lowest occupied bit with index >= from, or npos.
    std::size_t next_at_or_above(std::size_t from) const {
        if (from >= nbits_) return npos;
        std::size_t w = from >> 6;
        std::uint64_t b = words_[w] & shl(~0ull, from & 63);  // bits in this word >= from
        if (b) return (w << 6) | ctz(b);
        std::size_t sw = w >> 6;
        std::uint64_t sb = summary_[sw] & shl(~0ull, (w & 63) + 1);  // words in block > w
        while (true) {
            if (sb) {
                const std::size_t nw = (sw << 6) | ctz(sb);
                return (nw << 6) | ctz(words_[nw]);
            }
            if (++sw >= summary_.size()) return npos;
            sb = summary_[sw];
        }
    }

    // Highest occupied bit with index <= from, or npos. `from` must be < nbits_.
    std::size_t prev_at_or_below(std::size_t from) const {
        if (from >= nbits_) from = nbits_ ? nbits_ - 1 : npos;
        if (from == npos) return npos;
        std::size_t w = from >> 6;
        std::uint64_t b = words_[w] & shr(~0ull, 63 - (from & 63));  // bits in this word <= from
        if (b) return (w << 6) | msb(b);
        std::size_t sw = w >> 6;
        std::uint64_t sb = (w & 63) ? (summary_[sw] & shr(~0ull, 64 - (w & 63))) : 0;  // words < w
        while (true) {
            if (sb) {
                const std::size_t nw = (sw << 6) | msb(sb);
                return (nw << 6) | msb(words_[nw]);
            }
            if (sw == 0) return npos;
            --sw;
            sb = summary_[sw];
        }
    }

private:
    static std::uint64_t bit(std::size_t k) { return 1ull << k; }
    // Shifts guarded against the shift-by-64 UB (a full shift clears the word).
    static std::uint64_t shl(std::uint64_t v, std::size_t k) { return k >= 64 ? 0ull : v << k; }
    static std::uint64_t shr(std::uint64_t v, std::size_t k) { return k >= 64 ? 0ull : v >> k; }
    static std::size_t ctz(std::uint64_t v) { return static_cast<std::size_t>(__builtin_ctzll(v)); }
    static std::size_t msb(std::uint64_t v) { return 63 - static_cast<std::size_t>(__builtin_clzll(v)); }

    std::size_t nbits_;
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

}  // namespace ob
