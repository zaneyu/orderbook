# orderbook

**A price-time-priority limit order book / matching engine in C++20** — header-only,
**allocation-free on the hot path**, and checked against a naive reference by differential
fuzzing.

**▶ Live demo: https://orderbook.pages.dev** — the same engine compiled to WebAssembly,
matching a simulated market in your browser. The price process is a real one (stochastic
volatility, trends, fat-tailed news jumps); fire a news shock, turn up turbulence, click a
level to trade (your fills move the price), and watch your working orders fill. Source in
[`web/`](web/).

## Latency

Single-threaded, one core, warm cache (Apple M-series, `-O3 -march=native`, 1M orders
over a 65,536-tick book). `make bench`:

| op       | latency         | throughput      |
|----------|-----------------|-----------------|
| `rest`   | **47 ns**       | 21 M ops/s      |
| `cancel` | **75 ns**       | 13 M ops/s      |
| `match`  | **55 ns/fill**  | 18 M fills/s    |
| `mixed`  | **76 ns/op**    | 13 M ops/s      |

`mixed` is a realistic interleave of adds, cancels, and marketable orders — the number
that reflects normal use.

**Tail latency** matters more than the mean in trading. Per-op `cancel`, netting out the
clock call (steady_clock, unpinned thread): **p50 ~85 ns, p99 ~330 ns**. The p50→p99 spread
is cache-miss variance on random-order cancellation; the p99.9+ tail (µs) is OS scheduling,
not the algorithm — the hot path allocates nothing, so there are no malloc/rehash spikes.
Characterising the true far tail needs core isolation + a cycle counter; the benchmark is
honest that it doesn't do that.

## Operations

Limit, market, **IOC** (immediate-or-cancel), **FOK** (fill-or-kill), **modify**
(cancel/replace), and cancel — all price-time priority. `modify` keeps time priority on a
same-price size *decrease* and re-queues (loses priority, may cross) on any price change or
size increase, the standard exchange rule. Every one is checked against the reference in the
differential fuzz.

## Allocation-free hot path — and it's proven, not asserted

After a sized construction, `add_limit` / `cancel` / `add_market` perform **zero heap
allocations**. Order nodes come from a reused free list; the id→slot index is a custom
open-addressing flat map (`flat_id_map.hpp`) with backward-shift deletion — not a
node-based `std::unordered_map` that would `malloc` on every insert and `free` on every
erase. A test instruments global `new`/`delete` and asserts the counts are exactly zero
across 100k rests + 100k matches + 100k cancels (`tests/test_alloc_audit.cpp`).

## Is it correct? (the part that matters)

A matching engine is only as good as its correctness proof. The core is validated by a
**differential fuzz test**: the same stream of random operations is applied to the fast
engine *and* to a deliberately naive `ReferenceBook` (std::map + std::deque), and after
every operation their **fills and full book state are asserted identical** — across limit,
market, IOC, FOK, modify, and cancel. 180,000 ops across three seeds, plus targeted unit
scenarios per order type. The two supporting data structures are fuzzed the same way against
obvious references:

- `test_orderbook`  — engine vs naive reference book (differential fuzz + units)
- `test_occupancy`  — the best-pointer bitmap vs a brute-force `std::set`
- `test_flat_id_map`— the flat hash map vs `std::unordered_map` + the allocation audit
- `test_alloc_audit`— the whole engine's hot path is malloc-free

```bash
make test    # all four suites, incl. the differential fuzz  -> SUCCESS
```

CI runs every suite under **ASan + UBSan** and the benchmark on each push. The fuzz
passes clean under UBSan over the full 180k-op sweep.

## Design

An **array-indexed price ladder**. Prices are integer ticks in `[0, num_ticks)`. Each
side holds a `vector<Level>` indexed by tick; each `Level` is an **intrusive FIFO** of
order nodes drawn from a **reused pool**. Two structures keep every operation fast:

- **A two-level occupancy bitmap per side** (`occupancy.hpp`) marks which levels are
  non-empty, so moving the cached best-bid/ask to the next populated level is a
  bit-scan — no linear tick walk when a far level empties.
- **A flat open-addressing id map** (`flat_id_map.hpp`) gives O(1), allocation-free
  id→slot lookup for cancels and maker removal.

| operation  | cost                                                        |
|------------|-------------------------------------------------------------|
| `add_limit`| O(1) to rest; O(fills) when it crosses                      |
| `cancel`   | O(1) unlink + O(1) id-map erase + a bit-scan best refresh   |
| `match`    | O(1) per fill + a bit-scan best refresh per emptied level   |

The bit-scan touches at most ~`ticks/4096` summary words — effectively constant for any
realistic tick count, and with no linear-scan worst case (the flaw a plain ladder has
when the best level empties far from the next one).

## Usage

```cpp
#include "orderbook/order_book.hpp"
using namespace ob;

OrderBook book(/*num_ticks=*/1 << 16, /*reserve_orders=*/1'000'000);
std::vector<Trade> fills;

book.add_limit(1, Side::Sell, /*price=*/101, /*qty=*/10, fills);  // rests
book.add_limit(2, Side::Buy,  /*price=*/101, /*qty=*/6,  fills);  // crosses -> 1 fill of 6
// fills == { Trade{taker=2, maker=1, price=101, qty=6} }

book.best_ask();                 // 101  (4 left resting; valid only when has_ask())
book.modify(1, 101, 3, fills);   // shrink at same price -> keeps time priority
book.add_ioc(4, Side::Buy, 101, 100, fills);   // fills what crosses, discards the rest
book.add_fok(5, Side::Buy, 101, 100, fills);   // all-or-nothing (0 if filled, else qty)
book.cancel(1);                  // true
book.add_market(6, Side::Buy, 100, fills);  // sweeps the opposite side; never rests
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
include/orderbook/  order_book.hpp    the engine (array ladder + intrusive FIFO pool)
                    occupancy.hpp     two-level bitmap for O(1) best-level finding
                    flat_id_map.hpp   open-addressing id->slot map (allocation-free)
                    types.hpp         OrderId / Price / Qty / Side / Trade
tests/              test_orderbook.cpp   differential fuzz + unit scenarios
                    test_occupancy.cpp   bitmap vs brute force
                    test_flat_id_map.cpp map vs unordered_map + allocation audit
                    test_alloc_audit.cpp engine hot path is malloc-free
                    reference_book.hpp   naive oracle (std::map/deque)
                    framework.hpp        tiny zero-dep test harness
bench/              bench.cpp         rest / cancel / match / mixed microbenchmarks
```

## Scope & non-goals

Deliberately focused: single-threaded, single-symbol, integer tick prices, limit +
market orders with price-time priority. Accessors expose sentinels on an empty book
(`best_bid() == -1`, `best_ask() == num_ticks()`), valid only when `has_bid()/has_ask()`.
`Qty` is 32-bit and level totals are not saturated — fine within the intended range.
Natural extensions (not included): stop/pegged orders, per-symbol sharding, a lock-free
SPSC ingress, and L2/L3 snapshot streaming. The engine is the core; those are layers on
top of it.

## License

MIT.
