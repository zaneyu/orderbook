// Differential fuzz of FlatIdMap against std::unordered_map, plus an allocation
// audit proving that a correctly-sized map performs zero heap ops on the hot path.
#include <cstdint>
#include <cstdlib>
#include <new>
#include <random>
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
