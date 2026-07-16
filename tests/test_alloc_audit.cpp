// Measures the engine's REAL allocation envelope, not just its happy path:
//   1. a positive control proving the interception works at all
//   2. sized construction + any op mix (incl. deep multi-level sweeps into a
//      pre-reserved fill buffer): the engine performs ZERO heap allocations
//   3. a sweep into an UNDER-reserved fill buffer: the caller's vector grows —
//      that growth is the caller's, and the documented contract is that callers
//      on a latency budget reserve `out` for the deepest sweep they can see
//   4. an unsized (default) book: pool growth + index rehash allocate, amortized
// Global new/new[]/aligned new and matching deletes are all instrumented;
// counting is enabled only around the measured operations.
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
// Array and aligned forms too: the engine uses neither today, but an audit that
// only watches scalar new stays green vacuously if that ever changes.
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void* operator new(std::size_t n, std::align_val_t al) {
    if (g_count) ++g_new;
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(al), n ? n : 1) == 0) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept {
    if (g_count && p) ++g_del;
    std::free(p);
}
void operator delete(void* p, std::align_val_t al, std::size_t) noexcept {
    operator delete(p, al);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace ob;

TEST(alloc_counting_works) {
    // Positive control: if the replacement operators aren't linked in, every other
    // test here passes for the wrong reason. (A bare `new int` does NOT work here —
    // clang legally elides it at -O2, which this control originally caught.)
    g_new = g_del = 0;
    g_count = true;
    {
        std::vector<int> v;
        v.reserve(1);  // exactly one un-elidable allocation...
    }                  // ...freed here
    g_count = false;
    CHECK_EQ(g_new, std::size_t{1});
    CHECK_EQ(g_del, std::size_t{1});
}

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

TEST(deep_sweep_is_allocation_free_with_reserved_out) {
    // The old audit only ever produced 1-fill matches — the degenerate case that
    // cannot grow `out`. This is the advertised behavior: ONE marketable order
    // sweeping tens of thousands of makers across thousands of levels, and the
    // engine still adds nothing beyond the caller's (pre-reserved) buffer.
    constexpr Price TICKS = 1 << 16;
    constexpr Price MID = TICKS / 2;
    constexpr OrderId N = 50000;

    OrderBook book(TICKS, 2 * N);
    std::vector<Trade> out;
    for (OrderId k = 1; k <= N; ++k)
        book.add_limit(k, Side::Sell, static_cast<Price>(MID + (k % MID)), 10, out);
    out.clear();
    out.reserve(N + 16);  // sized to the worst-case sweep, per the documented contract

    g_new = g_del = 0;
    g_count = true;
    const Qty rem = book.add_market(9'999'999, Side::Buy, 10 * N, out);
    g_count = false;

    CHECK_EQ(rem, Qty{0});
    CHECK_EQ(out.size(), static_cast<std::size_t>(N));  // every maker filled
    CHECK_EQ(g_new, std::size_t{0});
    CHECK_EQ(g_del, std::size_t{0});
}

TEST(deep_sweep_grows_only_the_callers_buffer) {
    // Same sweep, under-reserved `out`: the vector reallocates (amortized doubling).
    // That growth belongs to the caller's buffer — it is why the contract says to
    // reserve `out` — and this test pins it as the ONLY allocation source.
    constexpr Price TICKS = 1 << 16;
    constexpr Price MID = TICKS / 2;
    constexpr OrderId N = 50000;

    OrderBook book(TICKS, 2 * N);
    std::vector<Trade> out;
    for (OrderId k = 1; k <= N; ++k)
        book.add_limit(k, Side::Sell, static_cast<Price>(MID + (k % MID)), 10, out);
    out.clear();
    out.shrink_to_fit();

    g_new = g_del = 0;
    g_count = true;
    book.add_market(9'999'999, Side::Buy, 10 * N, out);
    g_count = false;

    CHECK_EQ(out.size(), static_cast<std::size_t>(N));
    CHECK(g_new > 0);                 // the caller's vector grew — allocation is real
    CHECK(g_new <= 32);               // ...and amortized (doubling), not per-fill
}

TEST(unsized_book_allocates_amortized) {
    // The DEFAULT construction (reserve_orders = 0) is NOT malloc-free: pool growth
    // and id-index rehash allocate. Bounded and amortized, but real — "malloc-free"
    // holds only after sized construction, and this test documents that envelope.
    constexpr Price TICKS = 1 << 16;
    constexpr OrderId N = 100000;

    OrderBook book(TICKS);  // reserve_orders defaulted to 0
    std::vector<Trade> out;
    out.reserve(64);

    g_new = g_del = 0;
    g_count = true;
    for (OrderId k = 1; k <= N; ++k) {
        book.add_limit(k, Side::Buy, static_cast<Price>(k % (TICKS / 2)), 10, out);
        out.clear();
    }
    g_count = false;

    CHECK(g_new > 0);    // the default configuration allocates on the hot path
    CHECK(g_new <= 96);  // amortized doubling of pool + free list + id index
}

int main() { return tf::run(); }
