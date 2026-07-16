// Differential fuzz of FlatIdMap against std::unordered_map, an allocation audit
// proving that a correctly-sized map performs zero heap ops on the hot path, and a
// hash-flood test proving the per-instance seed actually participates in the hash.
#define OB_TESTING  // enables FlatIdMap::home_slot for the seed test
#include <cstdint>
#include <cstdlib>
#include <new>
#include <random>
#include <set>
#include <unordered_map>

#include "framework.hpp"
#include "orderbook/flat_id_map.hpp"

using namespace ob;

// ---- global allocation counters (for the malloc-free audit) -----------------
namespace {
std::size_t g_new = 0, g_del = 0;
bool g_count = false;
}  // namespace
// GCC's -Wmismatched-new-delete false-positives on a global new/delete pair that
// deliberately and consistently routes through malloc/free (what this audit needs).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    if (g_count) ++g_new;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (g_count && p) ++g_del;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

TEST(flat_map_matches_unordered_map) {
    FlatIdMap flat;
    std::unordered_map<OrderId, std::uint32_t> ref;
    std::mt19937_64 rng(7);
    int bad = 0;

    for (int step = 0; step < 200000 && !bad; ++step) {
        const OrderId k = rng() % 2000;  // dense-ish key space -> lots of collisions
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {
            const std::uint32_t v = static_cast<std::uint32_t>(rng());
            flat.set(k, v);
            ref[k] = v;
        } else if (op == 1) {
            if (flat.erase(k) != (ref.erase(k) > 0)) ++bad;
        }
        // query
        const auto it = ref.find(k);
        const std::uint32_t got = flat.get(k);
        if (it == ref.end()) {
            if (got != FlatIdMap::NIL) ++bad;
        } else if (got != it->second) {
            ++bad;
        }
        if (flat.size() != ref.size()) ++bad;
    }
    CHECK_EQ(bad, 0);
}

TEST(flat_map_survives_growth) {
    FlatIdMap m;  // default small capacity -> must grow
    for (OrderId k = 0; k < 5000; ++k) m.set(k, static_cast<std::uint32_t>(k));
    int bad = 0;
    for (OrderId k = 0; k < 5000; ++k)
        if (m.get(k) != static_cast<std::uint32_t>(k)) ++bad;
    CHECK_EQ(bad, 0);
    CHECK_EQ(m.size(), std::size_t{5000});
}

namespace {
// The forward splitmix64 finalizer (what FlatIdMap mixes with) and its published
// inverse. The test derives collision sets exactly the way an attacker would.
std::uint64_t fwd_mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
std::uint64_t inv_mix(std::uint64_t x) {
    x = (x ^ (x >> 31) ^ (x >> 62)) * 0x319642B2D24D8EC3ull;
    x = (x ^ (x >> 27) ^ (x >> 54)) * 0x96DE1B173F119089ull;
    x = x ^ (x >> 30) ^ (x >> 60);
    return x - 0x9E3779B97F4A7C15ull;
}
}  // namespace

TEST(hash_seed_defeats_precomputed_collisions) {
    // The unseeded finalizer is a public bijection: an attacker inverts it to mint ids
    // that all share one home bucket, collapsing probing to O(n) (~900x measured). The
    // per-instance seed is the defense — and this test is what notices if a refactor
    // silently drops it (a seedless mutant previously survived the ENTIRE suite).
    CHECK_EQ(inv_mix(fwd_mix(0x123456789ABCDEFull)), std::uint64_t{0x123456789ABCDEF});
    CHECK_EQ(fwd_mix(inv_mix(42)), std::uint64_t{42});

    constexpr int N = 48;
    FlatIdMap m(N);  // cap 128, mask 127 — matches the ctor's sizing rule for 48
    std::set<std::size_t> homes;
    for (int j = 0; j < N; ++j) {
        // id whose UNSEEDED hash lands in bucket 5 of a 128-slot table
        const OrderId id = inv_mix(5ull + (static_cast<std::uint64_t>(j) << 7));
        m.set(id, static_cast<std::uint32_t>(j));
        homes.insert(m.home_slot(id));
    }
    CHECK_EQ(m.size(), std::size_t{N});
    // Unseeded, all 48 ids share ONE home bucket. Seeded, they scatter like random
    // keys — demand well more than 1 without flaking (P(<=8 distinct of 128) ~ 0).
    CHECK(homes.size() > 8);
}

TEST(preallocated_map_is_allocation_free) {
    FlatIdMap m(100000);  // sized up front
    // warm-up done; now audit the hot path
    g_new = g_del = 0;
    g_count = true;
    for (OrderId k = 1; k <= 100000; ++k) m.set(k, static_cast<std::uint32_t>(k));  // inserts
    for (OrderId k = 1; k <= 100000; ++k) (void)m.get(k);                            // finds
    for (OrderId k = 1; k <= 100000; ++k) m.erase(k);                                // erases
    g_count = false;
    CHECK_EQ(g_new, std::size_t{0});
    CHECK_EQ(g_del, std::size_t{0});
}

int main() { return tf::run(); }
