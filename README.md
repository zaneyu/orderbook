# orderbook

**A price-time-priority limit order book / matching engine in C++20** — header-only,
allocation-free on the hot path, and checked against a naive reference by differential
fuzzing.

## Latency

Single-threaded, one core, warm cache (Apple M-series, `-O3 -march=native`, 1M orders
over a 65,536-tick book). `make bench`:

| op       | latency        | throughput      |
|----------|----------------|-----------------|
| `rest`   | **30 ns**      | 33 M ops/s      |
| `match`  | **103 ns/fill**| 9.7 M fills/s   |
| `mixed`  | **187 ns/op**  | 5.3 M ops/s     |
| `cancel` | 263 ns\*       | 3.8 M ops/s     |

\* `cancel` is measured under an *adversarial* pattern — every resting order cancelled
in fully random order — which maximises cache misses chasing intrusive-list pointers
through a 32 MB pool. The realistic `mixed` workload (adds, cancels, marketable orders
interleaved) is 187 ns/op; that's the number that reflects normal use.

## Is it correct? (the part that matters)

A matching engine is only as good as its correctness proof. This one is validated by a
**differential fuzz test**: the same stream of random operations is applied to the fast
engine *and* to a deliberately naive `ReferenceBook` (std::map + std::deque, nothing
clever to get wrong), and after every operation their **fills and full book state are
asserted identical**. 180,000 ops across three seeds, plus targeted unit scenarios
(price-time priority, partial fills, multi-level market sweeps, cancel-updates-best,
input rejection).

```bash
make test    # 11 tests incl. the differential fuzz     -> SUCCESS
make asan    # the same fuzz under AddressSanitizer + UBSan
```

CI runs the suite, the sanitizer pass (ASan + UBSan), and the benchmark on every push.
The fuzz already passes clean under UBSan over the full 180k-op sweep.

## Design

An **array-indexed price ladder**. Prices are integer ticks in `[0, num_ticks)`. Each
side holds a `vector<Level>` indexed by tick; each `Level` is an **intrusive FIFO** (a
doubly linked list) of order nodes drawn from a **reused pool** (a free list — no
`new`/`delete` on the hot path). Best bid / best ask are cached tick indices.

| operation  | cost                                                              |
|------------|-------------------------------------------------------------------|
| `add_limit`| O(1) to rest; O(fills) when it crosses                            |
| `cancel`   | O(1) unlink (+ amortised best-pointer walk if a best level empties)|
| `match`    | O(1) per fill                                                     |

The best-pointer walk on an emptied best level is O(ticks) worst case but amortised
away — the pointer only advances as levels are consumed, never revisiting a tick.

## Usage

```cpp
#include "orderbook/order_book.hpp"
using namespace ob;

OrderBook book(/*num_ticks=*/1 << 16, /*reserve_orders=*/1'000'000);
std::vector<Trade> fills;

book.add_limit(1, Side::Sell, /*price=*/101, /*qty=*/10, fills);  // rests
book.add_limit(2, Side::Buy,  /*price=*/101, /*qty=*/6,  fills);  // crosses -> 1 fill of 6
// fills == { Trade{taker=2, maker=1, price=101, qty=6} }

book.best_ask();                 // 101  (4 left resting)
book.cancel(1);                  // true
book.add_market(3, Side::Buy, 100, fills);  // sweeps the opposite side; never rests
```

Header-only: `#include "orderbook/order_book.hpp"` and add `include/` to your path.

## Build

```bash
make test && make bench          # or:
cmake -B build && cmake --build build && ctest --test-dir build
```

Requires a C++20 compiler. No dependencies.

## Layout

```
include/orderbook/  order_book.hpp   the engine (array ladder + intrusive FIFO pool)
                    types.hpp        OrderId / Price / Qty / Side / Trade
tests/              test_orderbook.cpp   unit scenarios + differential fuzz
                    reference_book.hpp   naive oracle (std::map/deque)
                    framework.hpp        tiny zero-dep test harness
bench/              bench.cpp        rest / cancel / match / mixed microbenchmarks
```

## Scope & non-goals

Deliberately focused: single-threaded, single-symbol, integer tick prices, limit +
market orders with price-time priority. Not included (natural extensions): IOC/FOK and
stop orders, per-symbol sharding, a lock-free SPSC ingress, and L2/L3 snapshot streaming.
The engine is the core; those are layers on top of it.

## License

MIT.
