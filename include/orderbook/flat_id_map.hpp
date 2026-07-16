#pragma once
//
// An open-addressing hash map from OrderId (uint64) to a pool slot (uint32),
// specialised for the order book's id index. Linear probing with backward-shift
// deletion (no tombstones), so probe chains stay tight and — once sized via the
// constructor — every insert/find/erase is allocation-free and cache-friendly.
// This is what makes the engine's hot path genuinely malloc-free, unlike a
// node-based std::unordered_map (one heap op per add/cancel/fill).
//
#include <atomic>
#include <cassert>
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
        // Keep load factor < ~0.7 for `expected_orders` without growing. The cap bound
        // stops `cap * 7` from wrapping mod 2^width (past cap = 2^(width-3) the product
        // wraps and the loop never terminates — reachable only via an absurd reserve,
        // but a hang is a hang). Width-aware: size_t is 32-bit under wasm32.
        constexpr std::size_t kMaxCap = std::size_t{1} << (sizeof(std::size_t) * 8 - 4);
        while (cap < kMaxCap && cap * 7 < (expected_orders + 1) * 10) cap <<= 1;
        cap_ = cap;
        mask_ = cap_ - 1;
        size_ = 0;
        keys_.assign(cap_, 0);
        vals_.assign(cap_, NIL);
        // Per-instance hash seed. The splitmix64 finalizer alone is a FIXED, publicly
        // invertible bijection: a caller who controls order ids can precompute a set
        // that collides on the table's low bits and collapse linear probing to O(n)
        // per op (measured ~900x in review). Mixing unpredictable per-instance state
        // into the key first makes that offline precomputation useless. ASLR'd `this`
        // plus a rotating counter is not cryptographic, but the attack requires the
        // seed and the seed never leaves process memory. (Under wasm there is no ASLR
        // and the heap is deterministic, so the seed is predictable there — fine for
        // the demo, whose clients don't choose order ids.) Hash choice does not affect
        // matching results, only probe distribution. The counter is atomic: each BOOK
        // is single-threaded, but independent books on independent threads (one per
        // symbol) may construct concurrently.
        static std::atomic<std::uint64_t> ctr{0};
        const std::uint64_t c = ctr.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed);
        seed_ = mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^ c);
    }

    std::size_t size() const { return size_; }
    bool contains(OrderId k) const { return probe(k) != cap_; }

#ifdef OB_TESTING
    // Test-only: the home bucket of `k` under this instance's seeded hash. Lets the
    // suite assert the seed actually participates (ids crafted to collide for the
    // UNSEEDED finalizer must scatter here) without exposing internals in production.
    std::size_t home_slot(OrderId k) const { return hash(k) & mask_; }
#endif

    // Slot for `k`, or NIL if absent.
    std::uint32_t get(OrderId k) const {
        const std::size_t i = probe(k);
        return i == cap_ ? NIL : vals_[i];
    }

    // Insert or overwrite. Grows (reallocates) only if the load factor would exceed
    // ~0.7 — which never happens after a correctly sized construction. Steady-state
    // allocation-freedom therefore requires passing `expected_orders` to the ctor.
    void set(OrderId k, std::uint32_t v) {
        assert(v != NIL);  // NIL aliases the empty sentinel and would silently vanish
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
    static std::uint64_t mix(std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    // Seeded per instance (see ctor) so collision sets can't be computed offline.
    std::uint64_t hash(std::uint64_t x) const { return mix(x ^ seed_); }

    std::vector<OrderId> keys_;
    std::vector<std::uint32_t> vals_;
    std::size_t cap_;
    std::size_t mask_;
    std::size_t size_;
    std::uint64_t seed_;
};

}  // namespace ob
