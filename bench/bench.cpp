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
    auto t0 = clk::now();
    for (std::size_t i = 0; i < N; ++i) {
        sink.clear();
        book.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink);
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
    }
    t1 = clk::now();
    row("match", t1 - t0, fills, "fill");

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
        ++ops;
    }
    t1 = clk::now();
    row("mixed", t1 - t0, ops, "op");

    // --- tail latency: per-op cancel distribution ------------------------------
    // Means hide the tail that trading actually cares about. Honest caveats: this is
    // steady_clock per op (coarse at this scale) on an UNPINNED thread. p50..p99
    // reflect the algorithm (the spread is cache-miss variance on random-order
    // cancellation); the p99.9+ tail is OS scheduling, not malloc (the hot path
    // allocates nothing). Real tail characterisation needs core isolation + a cycle
    // counter (rdtsc) -- out of scope for a portable microbench.
    {
        const std::size_t M = 200000;
        std::vector<std::uint32_t> ov(M);
        for (std::size_t i = 0; i < M; ++i) {
            const auto a = clk::now();
            const auto b = clk::now();
            ov[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        std::sort(ov.begin(), ov.end());
        const std::uint32_t clock_ns = ov[M / 2];  // net the clock-call cost out of ~70ns samples

        OrderBook lb(TICKS, N);
        for (std::size_t i = 0; i < N; ++i) {
            sink.clear();
            lb.add_limit(static_cast<OrderId>(i + 1), Side::Buy, prices[i], qtys[i], sink);
        }
        std::shuffle(order.begin(), order.end(), rng);
        std::vector<std::uint32_t> lat(N);
        for (std::size_t i = 0; i < N; ++i) {
            const auto a = clk::now();
            lb.cancel(static_cast<OrderId>(order[i] + 1));
            const auto b = clk::now();
            lat[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        std::sort(lat.begin(), lat.end());
        auto pct = [&](double q) {
            const long v = static_cast<long>(lat[static_cast<std::size_t>(q * (N - 1))]) - clock_ns;
            return v < 0 ? 0L : v;
        };
        std::printf("\n  cancel latency (ns, ~%u ns clock overhead netted out):\n", clock_ns);
        std::printf("    p50 %ld   p90 %ld   p99 %ld   p99.9 %ld   max %ld\n",
                    pct(0.50), pct(0.90), pct(0.99), pct(0.999),
                    static_cast<long>(lat.back()) - clock_ns);
    }

    return 0;
}
