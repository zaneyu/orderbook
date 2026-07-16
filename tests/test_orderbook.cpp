// Unit scenarios + a differential fuzz test: the same random op stream is applied to
// the fast OrderBook and the naive ReferenceBook, and their fills and full book state
// are asserted identical after every operation.
#include <cstdint>
#include <random>
#include <utility>
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

TEST(l2_snapshot_walks_populated_levels_from_best) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);
    b.add_limit(2, Side::Buy, 98, 3, fills);  // gap at 99
    b.add_limit(3, Side::Buy, 98, 2, fills);  // aggregates to 5 at 98
    std::vector<std::pair<Price, std::uint64_t>> lv;
    b.for_levels(Side::Buy, 10, [&](Price p, std::uint64_t q) { lv.emplace_back(p, q); });
    CHECK_EQ(lv.size(), std::size_t{2});
    CHECK_EQ(lv[0].first, Price{100});
    CHECK_EQ(lv[0].second, std::uint64_t{5});
    CHECK_EQ(lv[1].first, Price{98});   // empty 99 skipped
    CHECK_EQ(lv[1].second, std::uint64_t{5});

    b.add_limit(4, Side::Sell, 105, 4, fills);
    b.add_limit(5, Side::Sell, 107, 1, fills);
    std::vector<std::pair<Price, std::uint64_t>> la;
    b.for_levels(Side::Sell, 1, [&](Price p, std::uint64_t q) { la.emplace_back(p, q); });  // depth cap
    CHECK_EQ(la.size(), std::size_t{1});
    CHECK_EQ(la[0].first, Price{105});
}

TEST(l2_snapshot_at_tick_zero_terminates) {
    // A bid resting at tick 0 must END the walk, not wrap the cursor back to the top
    // of the ladder and silently re-visit levels (the occupancy clamp would allow it).
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 0, 5, fills);
    b.add_limit(2, Side::Buy, 2, 3, fills);
    std::vector<std::pair<Price, std::uint64_t>> lv;
    b.for_levels(Side::Buy, 10, [&](Price p, std::uint64_t q) { lv.emplace_back(p, q); });
    CHECK_EQ(lv.size(), std::size_t{2});
    CHECK_EQ(lv[0].first, Price{2});
    CHECK_EQ(lv[1].first, Price{0});
}

TEST(spread_is_ask_minus_bid) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);
    b.add_limit(2, Side::Sell, 103, 5, fills);
    CHECK_EQ(b.spread(), Price{3});
}

TEST(for_orders_walks_fifo_at_level) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);
    b.add_limit(2, Side::Buy, 100, 7, fills);
    std::vector<std::pair<OrderId, Qty>> fo;
    b.for_orders(Side::Buy, 100, [&](OrderId id, Qty q) { fo.emplace_back(id, q); });
    CHECK_EQ(fo.size(), std::size_t{2});
    CHECK_EQ(fo[0].first, OrderId{1});  // oldest first (time priority)
    CHECK_EQ(fo[0].second, Qty{5});
    CHECK_EQ(fo[1].first, OrderId{2});
    CHECK_EQ(fo[1].second, Qty{7});
}

TEST(level_total_beyond_u32_stays_exact) {
    // Many u32-sized orders at one price exceed a u32 aggregate. With a 32-bit total
    // this wrapped: qty_at reported 0 and FOK falsely killed a fillable order.
    OrderBook b(64);
    fills.clear();
    b.add_limit(1, Side::Sell, 5, 0x80000000u, fills);
    b.add_limit(2, Side::Sell, 5, 0x80000000u, fills);  // level total = 2^32 exactly
    CHECK_EQ(b.qty_at(Side::Sell, 5), std::uint64_t{0x100000000ull});
    CHECK(b.can_fill(Side::Buy, 5, 4000000000u));
    fills.clear();
    CHECK_EQ(b.add_fok(3, Side::Buy, 5, 1, fills), Qty{0});
    CHECK_EQ(fills.size(), std::size_t{1});
}

TEST(rest_only_rests_crossing_orders_without_matching) {
    // Feed-replay primitive: the venue already matched, so a crossed book is a valid
    // state to materialize (pre-open). rest_only must never generate fills, and later
    // matching must consume the crossed book in price-time order.
    OrderBook b(TICKS);
    fills.clear();
    CHECK(b.rest_only(1, Side::Sell, 100, 5));
    CHECK(b.rest_only(2, Side::Buy, 103, 4));  // crosses the resting ask: rests anyway
    CHECK(b.has_bid());
    CHECK_EQ(b.best_bid(), Price{103});
    CHECK_EQ(b.best_ask(), Price{100});  // crossed book, by design
    CHECK_EQ(b.qty_at(Side::Buy, 103), std::uint64_t{4});
    CHECK(!b.rest_only(1, Side::Buy, 50, 5));   // duplicate id rejected
    CHECK(!b.rest_only(9, Side::Buy, -1, 5));   // out of range rejected
    CHECK(!b.rest_only(9, Side::Buy, 100, 0));  // zero qty rejected
    fills.clear();
    b.add_market(9, Side::Sell, 4, fills);  // consumes the crossed bid at ITS price
    CHECK_EQ(fills.size(), std::size_t{1});
    CHECK_EQ(fills[0].maker, OrderId{2});
    CHECK_EQ(fills[0].price, Price{103});
}

TEST(front_order_returns_fifo_head) {
    OrderBook b(TICKS);
    fills.clear();
    OrderId f = 0;
    CHECK(!b.front_order(Side::Buy, 100, f));  // empty level
    CHECK(!b.front_order(Side::Buy, -1, f));   // out of range
    b.add_limit(7, Side::Buy, 100, 5, fills);
    b.add_limit(8, Side::Buy, 100, 5, fills);
    CHECK(b.front_order(Side::Buy, 100, f));
    CHECK_EQ(f, OrderId{7});  // oldest first
    fills.clear();
    b.add_limit(9, Side::Sell, 100, 5, fills);  // consumes id 7 exactly
    CHECK(b.front_order(Side::Buy, 100, f));
    CHECK_EQ(f, OrderId{8});  // head advanced
}

TEST(market_taker_id_may_collide_with_resting_id) {
    // market/IOC/FOK ids are tags on emitted fills, not indexed — a collision with a
    // resting order's id must not reject the taker.
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 99, 5, fills);
    b.add_limit(2, Side::Sell, 101, 5, fills);
    fills.clear();
    CHECK_EQ(b.add_market(1, Side::Buy, 3, fills), Qty{0});  // taker tag == resting id 1
    CHECK_EQ(fills.size(), std::size_t{1});
    CHECK_EQ(fills[0].taker, OrderId{1});
    CHECK_EQ(fills[0].maker, OrderId{2});
}

// ------------------------------------------------------ order-type unit scenarios

TEST(ioc_matches_then_discards) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 100, 6, fills);
    fills.clear();
    CHECK_EQ(b.add_ioc(2, Side::Buy, 100, 10, fills), Qty{4});  // 6 filled, 4 discarded
    CHECK_EQ(fills.size(), std::size_t{1});
    CHECK(!b.has_bid());  // remainder never rests
    CHECK(!b.has_ask());
}

TEST(fok_kills_when_insufficient) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 100, 6, fills);
    fills.clear();
    CHECK_EQ(b.add_fok(2, Side::Buy, 100, 10, fills), Qty{10});  // only 6 avail -> kill
    CHECK(fills.empty());
    CHECK_EQ(b.qty_at(Side::Sell, 100), Qty{6});  // untouched
}

TEST(fok_fills_across_levels_when_sufficient) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 100, 6, fills);
    b.add_limit(2, Side::Sell, 101, 6, fills);
    fills.clear();
    CHECK_EQ(b.add_fok(3, Side::Buy, 101, 10, fills), Qty{0});  // 12 avail within limit
    CHECK_EQ(fills.size(), std::size_t{2});
    CHECK_EQ(b.qty_at(Side::Sell, 101), Qty{2});
}

TEST(modify_decrease_keeps_priority) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 10, fills);  // older
    b.add_limit(2, Side::Buy, 100, 10, fills);  // younger
    CHECK(b.modify(1, 100, 5, fills));          // shrink same price -> keeps front slot
    // The level AGGREGATE must shrink too, not just the node: a mutant that dropped
    // the total update passed the whole suite because only later fills (which read
    // per-node qty) were asserted.
    CHECK_EQ(b.qty_at(Side::Buy, 100), std::uint64_t{15});
    fills.clear();
    b.add_limit(3, Side::Sell, 100, 6, fills);  // hits id1 (5) then id2 (1)
    CHECK_EQ(fills[0].maker, OrderId{1});
    CHECK_EQ(fills[0].qty, Qty{5});
    CHECK_EQ(fills[1].maker, OrderId{2});
    CHECK_EQ(b.qty_at(Side::Buy, 100), std::uint64_t{9});
}

TEST(can_fill_zero_qty_is_trivially_true) {
    OrderBook b(TICKS);
    CHECK(b.can_fill(Side::Buy, 100, 0));  // empty book: "fill nothing" still succeeds
    fills.clear();
    b.add_limit(1, Side::Sell, 100, 5, fills);
    CHECK(b.can_fill(Side::Buy, 100, 0));
    CHECK(b.can_fill(Side::Sell, 100, 0));
}

TEST(modify_increase_loses_priority) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);
    b.add_limit(2, Side::Buy, 100, 5, fills);
    CHECK(b.modify(1, 100, 8, fills));          // grow -> requeued behind id2
    fills.clear();
    b.add_limit(3, Side::Sell, 100, 5, fills);
    CHECK_EQ(fills[0].maker, OrderId{2});        // id2 now first
}

TEST(modify_reprice_can_cross) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Sell, 102, 5, fills);
    b.add_limit(2, Side::Buy, 100, 5, fills);
    fills.clear();
    CHECK(b.modify(2, 102, 5, fills));           // reprice bid up -> crosses the ask
    CHECK_EQ(fills.size(), std::size_t{1});
    CHECK_EQ(fills[0].maker, OrderId{1});
    CHECK(!b.has_ask());
}

TEST(modify_to_out_of_range_price_is_rejected_and_preserves_order) {
    OrderBook b(TICKS);
    fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, fills);
    // An out-of-range target price must NOT destroy the resting order.
    CHECK(!b.modify(1, TICKS, 5, fills));         // >= num_ticks -> rejected
    CHECK(!b.modify(1, -1, 5, fills));            // negative -> rejected
    CHECK_EQ(b.qty_at(Side::Buy, 100), Qty{5});   // order still resting, unchanged
    CHECK(b.cancel(1));                            // and still cancellable by id
}

// ------------------------------------------------------------ differential fuzz

namespace {

// Compare fast and reference book state exhaustively: emptiness, bests, live-order
// count, per-level aggregates, the per-order FIFO (id + remaining qty in time
// priority) at every populated level, and the L2 snapshot walk. Aggregate-only
// comparison let a real modify bug through: per-node qty stayed right while the
// level total corrupted, and nothing ever read the total.
bool same_state(const OrderBook& fast, const ReferenceBook& ref) {
    if (fast.has_bid() != ref.has_bid() || fast.has_ask() != ref.has_ask()) return false;
    if (fast.has_bid() && fast.best_bid() != ref.best_bid()) return false;
    if (fast.has_ask() && fast.best_ask() != ref.best_ask()) return false;
    if (fast.resting_orders() != ref.resting_orders()) return false;
    std::vector<std::pair<OrderId, Qty>> fo;
    for (const Side s : {Side::Buy, Side::Sell}) {
        for (Price p = 0; p < TICKS; ++p) {
            if (fast.qty_at(s, p) != ref.qty_at(s, p)) return false;
            if (fast.qty_at(s, p) == 0) continue;
            fo.clear();
            fast.for_orders(s, p, [&](OrderId id, Qty q) { fo.emplace_back(id, q); });
            if (fo != ref.orders_at(s, p)) return false;
        }
        // The L2 walk must agree too — for_levels was previously untested under fuzz,
        // and a mutant corrupting its ladder-edge termination survived the suite.
        std::vector<std::pair<Price, std::uint64_t>> fl;
        fast.for_levels(s, TICKS, [&](Price p, std::uint64_t q) { fl.emplace_back(p, q); });
        std::vector<std::pair<Price, std::uint64_t>> rl;
        if (s == Side::Buy && ref.has_bid())
            for (Price p = ref.best_bid(); p >= 0; --p) {
                if (const auto q = ref.qty_at(s, p)) rl.emplace_back(p, q);
            }
        if (s == Side::Sell && ref.has_ask())
            for (Price p = ref.best_ask(); p < TICKS; ++p) {
                if (const auto q = ref.qty_at(s, p)) rl.emplace_back(p, q);
            }
        if (fl != rl) return false;
    }
    return true;
}

void run_fuzz(std::uint64_t seed, int n_ops) {
    OrderBook fast(TICKS);
    ReferenceBook ref(TICKS);
    std::mt19937_64 rng(seed);
    struct LiveOrder { OrderId id; Price price; };
    std::vector<LiveOrder> live;  // resting ids + last-set price (pruned when found stale)
    OrderId next_id = 1;
    std::vector<Trade> fa, rb;
    int mismatches = 0;
    bool reported = false;

    auto pick_price = [&] { return static_cast<Price>(rng() % TICKS); };
    auto pick_qty = [&] { return static_cast<Qty>(1 + rng() % 50); };
    // ~6% of ops draw inputs the happy-path generators can never produce: ladder-edge
    // and out-of-range prices, zero and near-2^32 quantities, and id collisions.
    // Rejection paths that fuzz never reaches are rejection paths that don't work —
    // and huge quantities regression-test the u64 level totals.
    auto adversarial = [&] { return rng() % 16 == 0; };
    auto pick_adv_price = [&]() -> Price {
        switch (rng() % 5) {
            case 0: return -1;
            case 1: return 0;
            case 2: return TICKS - 1;
            case 3: return TICKS;
            default: return pick_price();
        }
    };
    auto pick_adv_qty = [&]() -> Qty {
        switch (rng() % 4) {
            case 0: return 0;
            case 1: return 0x80000000u;
            case 2: return 0xFFFFFFFFu;
            default: return pick_qty();
        }
    };

    for (int i = 0; i < n_ops && mismatches == 0; ++i) {
        const int roll = static_cast<int>(rng() % 100);
        fa.clear();
        rb.clear();
        const char* op = "?";
        if (roll < 40) {  // add limit (sometimes reusing an old id: dup ids must reject)
            op = "add_limit";
            const bool adv = adversarial();
            const OrderId id = (adv && !live.empty() && (rng() & 1))
                                   ? live[rng() % live.size()].id
                                   : next_id++;
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price price = adv ? pick_adv_price() : pick_price();
            const Qty qty = adv ? pick_adv_qty() : pick_qty();
            const Qty r1 = fast.add_limit(id, side, price, qty, fa);
            const Qty r2 = ref.add_limit(id, side, price, qty, rb);
            if (r1 != r2) ++mismatches;
            if (r1 > 0) live.push_back({id, price});
        } else if (roll < 46) {  // rest_only: feed-replay path, may create CROSSED books
            op = "rest_only";
            const bool adv = adversarial();
            const OrderId id = (adv && !live.empty() && (rng() & 1))
                                   ? live[rng() % live.size()].id
                                   : next_id++;
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price price = adv ? pick_adv_price() : pick_price();
            const Qty qty = adv ? pick_adv_qty() : pick_qty();
            const bool r1 = fast.rest_only(id, side, price, qty);
            const bool r2 = ref.rest_only(id, side, price, qty);
            if (r1 != r2) ++mismatches;
            if (r1) live.push_back({id, price});
            // every subsequent op then runs against possibly-crossed state — the
            // matching paths must consume a crossed book identically to the oracle
        } else if (roll < 52) {  // market (taker tag may collide with a resting id)
            op = "add_market";
            const OrderId id = (!live.empty() && rng() % 8 == 0) ? live[rng() % live.size()].id
                                                                 : next_id++;
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Qty qty = adversarial() ? pick_adv_qty() : pick_qty();
            if (fast.add_market(id, side, qty, fa) != ref.add_market(id, side, qty, rb))
                ++mismatches;
        } else if (roll < 64) {  // IOC
            op = "add_ioc";
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const bool adv = adversarial();
            const Price price = adv ? pick_adv_price() : pick_price();
            const Qty qty = adv ? pick_adv_qty() : pick_qty();
            if (fast.add_ioc(next_id, side, price, qty, fa) != ref.add_ioc(next_id, side, price, qty, rb))
                ++mismatches;
            ++next_id;
        } else if (roll < 76) {  // FOK
            op = "add_fok";
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const bool adv = adversarial();
            const Price price = adv ? pick_adv_price() : pick_price();
            const Qty qty = adv ? pick_adv_qty() : pick_qty();
            if (fast.add_fok(next_id, side, price, qty, fa) != ref.add_fok(next_id, side, price, qty, rb))
                ++mismatches;
            ++next_id;
        } else if (roll < 88 && !live.empty()) {  // modify
            op = "modify";
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k].id;
            Price np;
            Qty nq;
            if (rng() & 1) {
                // Half the modifies target the order's OWN price with a small qty: the
                // in-place-decrease branch. Uniform draws hit it with p ~ 1/TICKS —
                // measured 0 times in 180k ops, which is how a real bug there survived.
                np = live[k].price;
                nq = 1 + static_cast<Qty>(rng() % 8);
            } else {
                np = adversarial() ? pick_adv_price() : pick_price();
                nq = (rng() % 8 == 0) ? 0 : pick_qty();
            }
            const bool m1 = fast.modify(id, np, nq, fa);
            const bool m2 = ref.modify(id, np, nq, rb);
            if (m1 != m2) ++mismatches;
            if (!m1 || nq == 0) {  // stale id or modify-cancel: prune the entry
                live[k] = live.back();
                live.pop_back();
            } else if (np >= 0 && np < TICKS) {
                live[k].price = np;  // may have re-rested at the new price
            }
        } else if (!live.empty()) {  // cancel a (possibly stale) id
            op = "cancel";
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k].id;
            live[k] = live.back();
            live.pop_back();
            if (fast.cancel(id) != ref.cancel(id)) ++mismatches;
        }
        if (fa != rb) ++mismatches;
        // Full O(ticks) state comparison periodically (cheap per-op check is the fills
        // above); a divergence is still caught within the window and asserted below.
        if ((i & 15) == 0 && !same_state(fast, ref)) ++mismatches;
        if (mismatches && !reported) {  // make a CI fuzz failure debuggable from the log
            reported = true;
            std::printf("    fuzz divergence: seed=%llu op=%d (%s)\n",
                        static_cast<unsigned long long>(seed), i, op);
        }
    }
    if (!same_state(fast, ref)) ++mismatches;  // exhaustive final check
    CHECK_EQ(mismatches, 0);
}

}  // namespace

TEST(fuzz_matches_reference_seed1) { run_fuzz(1, kFuzzOps); }
TEST(fuzz_matches_reference_seed2) { run_fuzz(1337, kFuzzOps); }
TEST(fuzz_matches_reference_seed3) { run_fuzz(0xDEADBEEF, kFuzzOps); }

// CI also passes -DFUZZ_EXTRA_SEED=$GITHUB_RUN_ID: fixed seeds replay the identical
// trajectory forever, so CI-over-time adds no coverage without one varying seed.
// The seed is printed on divergence, so any failure is reproducible locally.
#ifdef FUZZ_EXTRA_SEED
TEST(fuzz_matches_reference_run_seed) { run_fuzz(FUZZ_EXTRA_SEED, kFuzzOps); }
#endif

int main() { return tf::run(); }
