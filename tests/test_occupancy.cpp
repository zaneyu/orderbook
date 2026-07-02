// Differential test of the two-level occupancy bitmap against a brute-force set.
#include <cstdint>
#include <random>
#include <set>

#include "framework.hpp"
#include "orderbook/occupancy.hpp"

using namespace ob;

namespace {

std::size_t brute_next(const std::set<std::size_t>& s, std::size_t from) {
    auto it = s.lower_bound(from);
    return it == s.end() ? Occupancy::npos : *it;
}
std::size_t brute_prev(const std::set<std::size_t>& s, std::size_t from) {
    auto it = s.upper_bound(from);  // first > from
    if (it == s.begin()) return Occupancy::npos;
    return *std::prev(it);
}

}  // namespace

TEST(occupancy_boundaries) {
    Occupancy occ(200);
    CHECK_EQ(occ.next_at_or_above(0), Occupancy::npos);
    CHECK_EQ(occ.prev_at_or_below(199), Occupancy::npos);
    occ.set(63);
    occ.set(64);
    occ.set(199);
    CHECK_EQ(occ.next_at_or_above(0), std::size_t{63});
    CHECK_EQ(occ.next_at_or_above(64), std::size_t{64});   // word boundary
    CHECK_EQ(occ.next_at_or_above(65), std::size_t{199});
    CHECK_EQ(occ.prev_at_or_below(199), std::size_t{199});
    CHECK_EQ(occ.prev_at_or_below(198), std::size_t{64});
    CHECK_EQ(occ.prev_at_or_below(63), std::size_t{63});
    occ.clear(64);
    CHECK_EQ(occ.next_at_or_above(64), std::size_t{199});
    CHECK_EQ(occ.prev_at_or_below(198), std::size_t{63});
}

TEST(occupancy_fuzz_vs_bruteforce) {
    const std::size_t N = 5000;  // spans many level-0 words and >1 summary word
    Occupancy occ(N);
    std::set<std::size_t> ref;
    std::mt19937_64 rng(99);
    int bad = 0;

    for (int step = 0; step < 40000 && !bad; ++step) {
        const std::size_t i = rng() % N;
        if (rng() & 1) {
            occ.set(i);
            ref.insert(i);
        } else {
            occ.clear(i);
            ref.erase(i);
        }
        const std::size_t q = rng() % N;
        if (occ.next_at_or_above(q) != brute_next(ref, q)) ++bad;
        if (occ.prev_at_or_below(q) != brute_prev(ref, q)) ++bad;
    }
    CHECK_EQ(bad, 0);
}

int main() { return tf::run(); }
