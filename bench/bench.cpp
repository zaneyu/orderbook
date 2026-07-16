// Microbenchmarks for the hot paths: rest (add without cross), cancel, and match.
// Single-threaded, warm cache, no allocation in the timed region (the pool is
// pre-grown). Reports ns/op and throughput. Numbers are indicative of the data
// structure, not a substitute for profiling your own workload.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

#include "orderbook/order_book.hpp"

using namespace ob;
using clk = std::chrono::steady_clock;

namespace {

constexpr Price TICKS = 1 << 16;
constexpr Price MID = TICKS / 2;

double ns_per(clk::duration d, std::size_t n) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / static_cast<double>(n);
}
void row(const char* name, clk::duration d, std::size_t n, const char* unit) {
    const double nsp = ns_per(d, n);
    std::printf("  %-8s %7.1f ns/%-5s  %8.2f M %s/s\n", name, nsp, unit, 1000.0 / nsp, unit);
}

// Compiler barrier: the book's ops mutate heap state so they can't be elided today,
// but a bench without this is one refactor away from timing nothing.
template <typename T>
inline void do_not_optimize(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// Per-op latency percentiles with the clock-call cost netted out (may be 0 on
// coarse clocks, in which case the percentiles are raw and quantized).
void percentiles(const char* name, std::vector<std::uint32_t>& lat, std::uint32_t clock_ns) {
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double q) {
        const long v = static_cast<long>(lat[static_cast<std::size_t>(q * (lat.size() - 1))]) - clock_ns;
        return v < 0 ? 0L : v;
    };
    const long mx = static_cast<long>(lat.back()) - clock_ns;
    std::printf("    %-12s p50 %5ld   p90 %5ld   p99 %5ld   p99.9 %6ld   max %ld\n", name,
                pct(0.50), pct(0.90), pct(0.99), pct(0.999), mx < 0 ? 0L : mx);
}

}  // namespace

int main() {
    const std::size_t N = 1'000'000;
    std::mt19937_64 rng(12345);

    std::vector<Price> prices(N);
    std::vector<Qty> qtys(N);
    for (std::size_t i = 0; i < N; ++i) {
        prices[i] = static_cast<Price>(rng() % MID);       // buys below MID => never cross
        qtys[i] = static_cast<Qty>(1 + rng() % 100);
    }
    std::vector<Trade> sink;
    sink.reserve(256);

    std::printf("order-book microbenchmarks  (N = %zu, %d ticks)\n\n", N, TICKS);

    // --- rest: add limit orders that never cross (pure insert path) --------------
    OrderBook book(TICKS, N);
    // Warmup: touch every pool page and ladder line once (add + cancel N), so the
    // timed region measures steady state, not first-touch page faults on 30MB of
    // freshly reserved pool.
    for (std::size_t i = 0; i < N; ++i) {
        sink.clear();
        book.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink);
    }
    for (std::size_t i = 0; i < N; ++i) book.cancel(static_cast<OrderId>(i + 1));
    auto t0 = clk::now();
    for (std::size_t i = 0; i < N; ++i) {
        sink.clear();
        book.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink);
        do_not_optimize(sink);
    }
    auto t1 = clk::now();
    row("rest", t1 - t0, N, "op");

    // --- cancel: remove all resting orders in shuffled order --------------------
    std::vector<std::size_t> order(N);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), rng);
    t0 = clk::now();
    for (std::size_t i = 0; i < N; ++i) book.cancel(static_cast<OrderId>(order[i] + 1));
    t1 = clk::now();
    row("cancel", t1 - t0, N, "op");

    // --- match: rest a deep sell book, then sweep it with market buys -----------
    OrderBook mbook(TICKS, N);
    for (std::size_t i = 0; i < N; ++i)
        mbook.add_limit(static_cast<OrderId>(i + 1), Side::Sell,
                        static_cast<Price>(MID + rng() % MID), qtys[i], sink), sink.clear();
    std::size_t fills = 0;
    t0 = clk::now();
    OrderId taker = N + 1;
    while (mbook.has_ask()) {
        sink.clear();
        mbook.add_market(taker++, Side::Buy, 40, sink);  // ~1-2 makers per taker
        fills += sink.size();
        do_not_optimize(fills);
    }
    t1 = clk::now();
    row("match", t1 - t0, fills, "fill");

    // --- sweep: ONE marketable order walking thousands of levels ----------------
    // The `match` mix above is shallow (~1 level/taker) and never stresses the
    // per-emptied-level occupancy refresh; this is the deep worst case.
    {
        OrderBook swbook(TICKS, N);
        std::size_t placed = 0;
        for (Price p = MID; p < TICKS - 1 && placed < N; ++p, ++placed)  // 1 maker/level
            swbook.add_limit(static_cast<OrderId>(placed + 1), Side::Sell, p, 10, sink), sink.clear();
        std::vector<Trade> big;
        big.reserve(placed + 16);  // engine contract: reserve `out` for the sweep depth
        t0 = clk::now();
        swbook.add_market(static_cast<OrderId>(placed + 2), Side::Buy,
                          static_cast<Qty>(10 * placed), big);
        t1 = clk::now();
        do_not_optimize(big);
        row("sweep", t1 - t0, big.size(), "fill");
    }

    // --- mixed: realistic interleave of add / cancel / marketable ---------------
    OrderBook xbook(TICKS, N);
    std::vector<OrderId> livev;
    livev.reserve(N);
    OrderId id = 1;
    std::size_t ops = 0;
    t0 = clk::now();
    for (std::size_t i = 0; i < N; ++i) {
        sink.clear();
        const int r = static_cast<int>(rng() % 100);
        if (r < 60 || livev.empty()) {
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price base = (s == Side::Buy) ? 0 : MID;
            const OrderId oid = id++;
            if (xbook.add_limit(oid, s, static_cast<Price>(base + rng() % MID), 1 + rng() % 50, sink) > 0)
                livev.push_back(oid);
        } else if (r < 85) {
            const std::size_t k = rng() % livev.size();
            xbook.cancel(livev[k]);
            livev[k] = livev.back();
            livev.pop_back();
        } else {
            xbook.add_market(id++, (rng() & 1) ? Side::Buy : Side::Sell, 1 + rng() % 30, sink);
        }
        do_not_optimize(sink);
        ++ops;
    }
    t1 = clk::now();
    row("mixed", t1 - t0, ops, "op");

    // --- tail latency: per-op distributions -------------------------------------
    // Means hide the tail that trading actually cares about. Honest caveats: this is
    // steady_clock per op (coarse at this scale) on an UNPINNED thread. p50..p99
    // reflect the algorithm; the p99.9+ tail is OS scheduling, not malloc (the hot
    // path allocates nothing). Real tail characterisation needs core isolation + a
    // cycle counter (rdtsc) -- out of scope for a portable microbench.
    {
        const std::size_t M = 200000;
        std::vector<std::uint32_t> ov(M);
        for (std::size_t i = 0; i < M; ++i) {
            const auto a = clk::now();
            const auto b = clk::now();
            ov[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        std::sort(ov.begin(), ov.end());
        const std::uint32_t clock_ns = ov[M / 2];  // net the clock-call cost out (may be 0)

        std::printf("\n  per-op latency (ns, ~%u ns clock overhead netted out):\n", clock_ns);
        std::vector<std::uint32_t> lat(N);

        // rest: pure insert, warm book
        {
            OrderBook lb(TICKS, N);
            for (std::size_t i = 0; i < N; ++i)
                lb.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink), sink.clear();
            for (std::size_t i = 0; i < N; ++i) lb.cancel(static_cast<OrderId>(i + 1));
            for (std::size_t i = 0; i < N; ++i) {
                sink.clear();
                const auto a = clk::now();
                lb.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink);
                const auto b = clk::now();
                lat[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
            }
            percentiles("rest", lat, clock_ns);
        }

        // cancel, dense book: ~30 orders/level, so a cancel almost never empties a
        // level — this is the CHEAP path (unlink + id-map erase, no best refresh).
        {
            OrderBook lb(TICKS, N);
            for (std::size_t i = 0; i < N; ++i) {
                sink.clear();
                lb.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink);
            }
            std::shuffle(order.begin(), order.end(), rng);
            for (std::size_t i = 0; i < N; ++i) {
                const auto a = clk::now();
                lb.cancel(static_cast<OrderId>(order[i] + 1));
                const auto b = clk::now();
                lat[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
            }
            percentiles("cancel", lat, clock_ns);
        }

        // cancel-best, sparse book: one order per populated level with gaps, always
        // cancelling the current best — every cancel empties the best level and pays
        // the occupancy-bitmap walk the dense workload designs out. This is the
        // advertised "near-O(1) best refresh" actually being measured.
        {
            OrderBook lb(TICKS, N);
            std::size_t placed = 0;
            std::vector<OrderId> ids;
            for (Price p = MID - 1; p >= 8 && placed < N; p -= 8, ++placed) {  // sparse: 1 per 8 ticks
                sink.clear();
                lb.add_limit(static_cast<OrderId>(placed + 1), Side::Buy, p, 10, sink);
                ids.push_back(static_cast<OrderId>(placed + 1));
            }
            std::vector<std::uint32_t> lat2(placed);
            for (std::size_t i = 0; i < placed; ++i) {  // ids were placed best-first
                const auto a = clk::now();
                lb.cancel(ids[i]);
                const auto b = clk::now();
                lat2[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
            }
            percentiles("cancel-best", lat2, clock_ns);
        }
    }

    return 0;
}
