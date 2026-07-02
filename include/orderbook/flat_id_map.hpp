#pragma once
//
// An open-addressing hash map from OrderId (uint64) to a pool slot (uint32),
// specialised for the order book's id index. Linear probing with backward-shift
// deletion (no tombstones), so probe chains stay tight and — once sized via the
// constructor — every insert/find/erase is allocation-free and cache-friendly.
// This is what makes the engine's hot path genuinely malloc-free, unlike a
// node-based std::unordered_map (one heap op per add/cancel/fill).
//
#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace ob {

class FlatIdMap {
public:
    static constexpr std::uint32_t NIL = 0xFFFFFFFFu;  // empty slot / "not found" value

    explicit FlatIdMap(std::size_t expected_orders = 0) {
        std::size_t cap = 16;
        // keep load factor < ~0.7 for `expected_orders` without growing
        while (cap * 7 < (expected_orders + 1) * 10) cap <<= 1;
        cap_ = cap;
        mask_ = cap_ - 1;
        size_ = 0;
        keys_.assign(cap_, 0);
        vals_.assign(cap_, NIL);
    }

    std::size_t size() const { return size_; }
    bool contains(OrderId k) const { return probe(k) != cap_; }

    // Slot for `k`, or NIL if absent.
    std::uint32_t get(OrderId k) const {
        const std::size_t i = probe(k);
        return i == cap_ ? NIL : vals_[i];
    }

    // Insert or overwrite. Grows (reallocates) only if the load factor would exceed
    // ~0.7 — never happens after a correctly sized construction.
    void set(OrderId k, std::uint32_t v) {
        if ((size_ + 1) * 10 >= cap_ * 7) grow();
        std::size_t i = hash(k) & mask_;
        while (vals_[i] != NIL) {
            if (keys_[i] == k) { vals_[i] = v; return; }
            i = (i + 1) & mask_;
        }
        keys_[i] = k;
        vals_[i] = v;
        ++size_;
    }

    // Remove `k`; returns true if present. Backward-shift keeps the table tombstone-free.
    bool erase(OrderId k) {
        std::size_t i = probe(k);
        if (i == cap_) return false;
        std::size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (vals_[j] == NIL) break;
            const std::size_t h = hash(keys_[j]) & mask_;
            // Leave entry j if its home h lies cyclically in (i, j]; else shift it into the hole.
            const bool keep = (i < j) ? (h > i && h <= j) : (h > i || h <= j);
            if (!keep) {
                keys_[i] = keys_[j];
                vals_[i] = vals_[j];
                i = j;
            }
        }
        vals_[i] = NIL;
        --size_;
        return true;
    }

private:
    std::size_t probe(OrderId k) const {
        std::size_t i = hash(k) & mask_;
        while (vals_[i] != NIL) {
            if (keys_[i] == k) return i;
            i = (i + 1) & mask_;
        }
        return cap_;  // sentinel "not found"
    }

    void grow() {
        std::vector<OrderId> ok = std::move(keys_);
        std::vector<std::uint32_t> ov = std::move(vals_);
        cap_ <<= 1;
        mask_ = cap_ - 1;
        size_ = 0;
        keys_.assign(cap_, 0);
        vals_.assign(cap_, NIL);
        for (std::size_t x = 0; x < ov.size(); ++x)
            if (ov[x] != NIL) insert_fresh(ok[x], ov[x]);
    }
    void insert_fresh(OrderId k, std::uint32_t v) {
        std::size_t i = hash(k) & mask_;
        while (vals_[i] != NIL) i = (i + 1) & mask_;
        keys_[i] = k;
        vals_[i] = v;
        ++size_;
    }

    // splitmix64 finalizer: strong avalanche for integer keys.
    static std::uint64_t hash(std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    std::vector<OrderId> keys_;
    std::vector<std::uint32_t> vals_;
    std::size_t cap_;
    std::size_t mask_;
    std::size_t size_;
};

}  // namespace ob
