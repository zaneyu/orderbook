// Proves the central claim: after a sized construction, the engine performs ZERO
// heap allocations across the hot path (rest, match, cancel). Global new/delete are
// instrumented; counting is enabled only around the measured operations.
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "framework.hpp"
#include "orderbook/order_book.hpp"

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

using namespace ob;

TEST(engine_hot_path_is_allocation_free) {
    constexpr Price TICKS = 1 << 16;
    constexpr Price MID = TICKS / 2;
    constexpr OrderId N = 100000;

    OrderBook book(TICKS, /*reserve_orders=*/3 * N);
    std::vector<Trade> out;
    out.reserve(4096);  // pre-sized so match fills never reallocate this scratch vector

    g_new = g_del = 0;
    g_count = true;
    // rest N buys below MID and N sells above MID (no crossing)
    for (OrderId k = 1; k <= N; ++k) {
        book.add_limit(k, Side::Buy, static_cast<Price>(k % MID), 10, out);
        out.clear();
    }
    for (OrderId k = N + 1; k <= 2 * N; ++k) {
        book.add_limit(k, Side::Sell, static_cast<Price>(MID + (k % MID)), 10, out);
        out.clear();
    }
    // match: market buys sweep the asks (one fill each)
    OrderId taker = 10 * N;
    for (OrderId i = 0; i < N; ++i) {
        book.add_market(taker++, Side::Buy, 10, out);
        out.clear();
    }
    // cancel the remaining resting buys
    for (OrderId k = 1; k <= N; ++k) book.cancel(k);
    g_count = false;

    CHECK_EQ(g_new, std::size_t{0});
    CHECK_EQ(g_del, std::size_t{0});
    CHECK_EQ(book.resting_orders(), std::size_t{0});
}

int main() { return tf::run(); }
