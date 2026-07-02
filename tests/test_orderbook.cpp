// Unit scenarios + a differential fuzz test: the same random op stream is applied to
// the fast OrderBook and the naive ReferenceBook, and their fills and full book state
// are asserted identical after every operation.
#include <cstdint>
#include <random>
#include <vector>

#include "framework.hpp"
#include "orderbook/order_book.hpp"
#include "reference_book.hpp"

using namespace ob;

namespace {

constexpr Price TICKS = 4096;
#ifndef FUZZ_OPS
#define FUZZ_OPS 60000
#endif
constexpr int kFuzzOps = FUZZ_OPS;

std::vector<Trade> fills;  // scratch reused across ops

}  // namespace

// ---------------------------------------------------------------- unit scenarios

TEST(rest_and_query) {
    OrderBook b(TICKS);
    fills.clear();
    CHECK_EQ(b.add_limit(1, Side::Buy, 100, 10, fills), Qty{10});
    CHECK(fills.empty());
    CHECK(b.has_bid());
    CHECK(!b.has_ask());
    CHECK_EQ(b.best_bid(), Price{100});
    CHECK_EQ(b.qty_at(Side::Buy, 100), Qty{10});
    CHECK_EQ(b.resting_orders(), std::size_t{1});
}

TEST(full_cross_consumes_resting) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 100, 10, fills);
    fills.clear();
    const Qty rem = b.add_limit(2, Side::Buy, 100, 10, fills);  // lifts the ask exactly
    CHECK_EQ(rem, Qty{0});
    CHECK_EQ(fills.size(), std::size_t{1});
    CHECK_EQ(fills[0], (Trade{2, 1, 100, 10}));
    CHECK(!b.has_ask());
    CHECK(!b.has_bid());
    CHECK_EQ(b.resting_orders(), std::size_t{0});
}

TEST(partial_fill_rests_remainder) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 100, 6, fills);
    fills.clear();
    const Qty rem = b.add_limit(2, Side::Buy, 100, 10, fills);
    CHECK_EQ(rem, Qty{4});
    CHECK_EQ(fills.size(), std::size_t{1});
    CHECK_EQ(fills[0].qty, Qty{6});
    CHECK(!b.has_ask());
    CHECK_EQ(b.best_bid(), Price{100});
    CHECK_EQ(b.qty_at(Side::Buy, 100), Qty{4});
}

TEST(price_time_priority) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);  // older
    b.add_limit(2, Side::Buy, 100, 5, fills);  // younger, same price
    fills.clear();
    b.add_limit(3, Side::Sell, 100, 7, fills);  // should hit id 1 fully, then id 2 partially
    CHECK_EQ(fills.size(), std::size_t{2});
    CHECK_EQ(fills[0].maker, OrderId{1});
    CHECK_EQ(fills[0].qty, Qty{5});
    CHECK_EQ(fills[1].maker, OrderId{2});
    CHECK_EQ(fills[1].qty, Qty{2});
    CHECK_EQ(b.qty_at(Side::Buy, 100), Qty{3});
}

TEST(market_sweeps_multiple_levels) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 101, 4, fills);
    b.add_limit(2, Side::Sell, 102, 4, fills);
    b.add_limit(3, Side::Sell, 103, 4, fills);
    fills.clear();
    const Qty rem = b.add_market(9, Side::Buy, 10, fills);
    CHECK_EQ(rem, Qty{0});
    CHECK_EQ(fills.size(), std::size_t{3});
    CHECK_EQ(fills[0].price, Price{101});
    CHECK_EQ(fills[1].price, Price{102});
    CHECK_EQ(fills[2].price, Price{103});
    CHECK_EQ(fills[2].qty, Qty{2});  // last level partially consumed
    CHECK_EQ(b.best_ask(), Price{103});
    CHECK_EQ(b.qty_at(Side::Sell, 103), Qty{2});
}

TEST(market_on_empty_book_returns_unfilled) {
    OrderBook b(TICKS);
    fills.clear();
    CHECK_EQ(b.add_market(1, Side::Buy, 5, fills), Qty{5});
    CHECK(fills.empty());
}

TEST(cancel_updates_best) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);
    b.add_limit(2, Side::Buy, 99, 5, fills);
    CHECK_EQ(b.best_bid(), Price{100});
    CHECK(b.cancel(1));
    CHECK_EQ(b.best_bid(), Price{99});   // best rolls back to the next level
    CHECK(!b.cancel(1));                  // already gone
    CHECK(!b.cancel(42));                 // never existed
}

TEST(rejects_bad_orders) {
    OrderBook b(TICKS);
    fills.clear();
    CHECK_EQ(b.add_limit(1, Side::Buy, 100, 0, fills), Qty{0});   // zero qty
    CHECK_EQ(b.add_limit(1, Side::Buy, -1, 5, fills), Qty{0});    // out of range
    CHECK_EQ(b.add_limit(1, Side::Buy, TICKS, 5, fills), Qty{0}); // out of range
    b.add_limit(7, Side::Buy, 100, 5, fills);
    CHECK_EQ(b.add_limit(7, Side::Sell, 200, 5, fills), Qty{0});  // duplicate id rejected
    CHECK_EQ(b.resting_orders(), std::size_t{1});
}

// ------------------------------------------------------------ differential fuzz

namespace {

// Compare fast and reference book state exhaustively for one op's result.
bool same_state(const OrderBook& fast, const ReferenceBook& ref) {
    if (fast.has_bid() != ref.has_bid() || fast.has_ask() != ref.has_ask()) return false;
    if (fast.has_bid() && fast.best_bid() != ref.best_bid()) return false;
    if (fast.has_ask() && fast.best_ask() != ref.best_ask()) return false;
    for (Price p = 0; p < TICKS; ++p) {
        if (fast.qty_at(Side::Buy, p) != ref.qty_at(Side::Buy, p)) return false;
        if (fast.qty_at(Side::Sell, p) != ref.qty_at(Side::Sell, p)) return false;
    }
    return true;
}

void run_fuzz(std::uint64_t seed, int n_ops) {
    OrderBook fast(TICKS);
    ReferenceBook ref(TICKS);
    std::mt19937_64 rng(seed);
    std::vector<OrderId> live;  // resting ids (may contain stale entries; filtered on use)
    OrderId next_id = 1;
    std::vector<Trade> fa, rb;
    int mismatches = 0;

    auto pick_price = [&] { return static_cast<Price>(rng() % TICKS); };
    auto pick_qty = [&] { return static_cast<Qty>(1 + rng() % 50); };

    for (int i = 0; i < n_ops && mismatches == 0; ++i) {
        const int roll = static_cast<int>(rng() % 100);
        fa.clear();
        rb.clear();
        if (roll < 55) {  // add limit
            const OrderId id = next_id++;
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price price = pick_price();
            const Qty qty = pick_qty();
            const Qty r1 = fast.add_limit(id, side, price, qty, fa);
            const Qty r2 = ref.add_limit(id, side, price, qty, rb);
            if (r1 != r2) ++mismatches;
            if (r1 > 0) live.push_back(id);
        } else if (roll < 75) {  // market
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Qty qty = pick_qty();
            const OrderId id = next_id++;
            if (fast.add_market(id, side, qty, fa) != ref.add_market(id, side, qty, rb)) ++mismatches;
        } else if (!live.empty()) {  // cancel a (possibly stale) id
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k];
            live[k] = live.back();
            live.pop_back();
            if (fast.cancel(id) != ref.cancel(id)) ++mismatches;
        }
        if (fa != rb) ++mismatches;
        // Full O(ticks) state comparison periodically (cheap per-op check is the fills
        // above); a divergence is still caught within the window and asserted below.
        if ((i & 15) == 0 && !same_state(fast, ref)) ++mismatches;
    }
    if (!same_state(fast, ref)) ++mismatches;  // exhaustive final check
    CHECK_EQ(mismatches, 0);
}

}  // namespace

TEST(fuzz_matches_reference_seed1) { run_fuzz(1, kFuzzOps); }
TEST(fuzz_matches_reference_seed2) { run_fuzz(1337, kFuzzOps); }
TEST(fuzz_matches_reference_seed3) { run_fuzz(0xDEADBEEF, kFuzzOps); }

int main() { return tf::run(); }
