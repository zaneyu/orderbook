#pragma once
//
// A price-time-priority limit order book / matching engine.
//
// Design: an array-indexed price ladder. Prices are integer ticks in [0, num_ticks).
// Each side keeps a vector<Level> indexed by tick; each Level is an intrusive FIFO
// (doubly linked list) of order nodes drawn from a reused pool. A two-level occupancy
// bitmap per side (see occupancy.hpp) tracks which levels are non-empty, so moving the
// cached best-bid / best-ask pointer to the next populated level is near-O(1) instead
// of an O(ticks) linear scan.
//
//   add_limit : O(1) to rest; O(fills) when it crosses
//   cancel    : O(1) unlink + near-O(1) best refresh when a best level empties
//   match     : O(1) per fill + near-O(1) best refresh per emptied level
//
// Allocation contract: once constructed with `reserve_orders` >= peak live orders, the
// ENGINE performs no heap allocations of its own on add/cancel/match — nodes come from
// a free list and the id->slot index is an open-addressing flat map (flat_id_map.hpp).
// Fills are appended to the CALLER-provided `out` vector, which grows like any vector:
// a taker sweeping N makers appends N trades, so callers on a latency budget must
// reserve `out` for the deepest sweep they can see. Under-reserving `reserve_orders`
// (or the default 0) allocates amortized on pool growth / index rehash.
// tests/test_alloc_audit.cpp measures exactly these envelopes.
//
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "flat_id_map.hpp"
#include "occupancy.hpp"
#include "types.hpp"

namespace ob {

class OrderBook {
public:
    // Price domain is integer ticks in [0, num_ticks). Orders outside are rejected.
    // `reserve_orders` pre-sizes the node pool and id index so steady-state operation
    // never rehashes or reallocates (recommended for latency-sensitive use).
    explicit OrderBook(Price num_ticks, std::size_t reserve_orders = 0)
        : num_ticks_((num_ticks < 0 ? throw std::invalid_argument("OrderBook: num_ticks < 0")
                                    : num_ticks)),
          bid_levels_(static_cast<std::size_t>(num_ticks)),
          ask_levels_(static_cast<std::size_t>(num_ticks)),
          bid_occ_(static_cast<std::size_t>(num_ticks)),
          ask_occ_(static_cast<std::size_t>(num_ticks)),
          best_bid_(-1),
          best_ask_(num_ticks),
          id_map_(reserve_orders) {
        if (reserve_orders) {
            pool_.reserve(reserve_orders);
            free_.reserve(reserve_orders);
        }
    }

    // Add a limit order. Any quantity that crosses the book matches immediately at
    // resting prices (price-time priority); the remainder rests. Fills are appended
    // to `out`. Returns the resting (unfilled) quantity. A duplicate/known id or an
    // out-of-range/zero-qty order is rejected (returns 0, no fills, no state change).
    Qty add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_ || id_map_.contains(id)) return 0;
        Qty rem = (side == Side::Buy) ? match<true>(id, price, qty, out)
                                      : match<false>(id, price, qty, out);
        if (rem > 0) rest(id, side, price, rem);
        return rem;
    }

    // Market order: match up to `qty` against the opposite side; never rests. Returns
    // the unfilled quantity (nonzero only if the book ran dry). `id` is used only as the
    // taker tag on emitted fills; since a market order never rests it is not indexed and
    // not checked for uniqueness.
    Qty add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& out) {
        if (qty == 0) return 0;
        return (side == Side::Buy) ? match<true>(id, num_ticks_ - 1, qty, out)
                                   : match<false>(id, 0, qty, out);
    }

    // Immediate-or-cancel: match whatever crosses now; discard the rest (never rests).
    // Returns the unfilled (cancelled) quantity.
    Qty add_ioc(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_) return qty;
        return (side == Side::Buy) ? match<true>(id, price, qty, out)
                                   : match<false>(id, price, qty, out);
    }

    // Fill-or-kill: execute in full immediately or not at all. Returns 0 when filled,
    // otherwise `qty` (killed — no fills, no state change).
    Qty add_fok(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_) return qty;
        if (!can_fill(side, price, qty)) return qty;
        return (side == Side::Buy) ? match<true>(id, price, qty, out)   // can_fill => fully fills
                                   : match<false>(id, price, qty, out);
    }

    // Modify a resting order (cancel/replace). A size *decrease at the same price*
    // keeps time priority (in-place); any price change or size increase loses it (the
    // order is re-queued at the back of the new level, and may cross). new_qty == 0
    // cancels. Returns false if `id` is not resting.
    bool modify(OrderId id, Price new_price, Qty new_qty, std::vector<Trade>& out) {
        const std::uint32_t slot = id_map_.get(id);
        if (slot == FlatIdMap::NIL) return false;
        const Node& n = pool_[slot];
        if (new_qty != 0 && new_price == n.price && new_qty <= n.qty) {
            const Qty diff = n.qty - new_qty;
            (n.side == Side::Buy ? bid_levels_ : ask_levels_)[static_cast<std::size_t>(n.price)].total -= diff;
            pool_[slot].qty = new_qty;
            return true;
        }
        const Side side = n.side;
        if (new_qty == 0) {  // documented: new_qty == 0 cancels
            cancel(id);
            return true;
        }
        // An out-of-range target price is an invalid modify: leave the resting order
        // untouched and report failure — never cancel it and silently drop it.
        if (new_price < 0 || new_price >= num_ticks_) return false;
        cancel(id);
        add_limit(id, side, new_price, new_qty, out);
        return true;
    }

    // True if a `side` taker at `price` could be fully filled (>= qty) immediately.
    bool can_fill(Side taker, Price limit, Qty qty) const {
        if (qty == 0) return true;  // "fill nothing" is trivially satisfiable, book or no book
        std::uint64_t acc = 0;
        if (taker == Side::Buy) {
            for (Price p = best_ask_; p < num_ticks_ && p <= limit;) {
                acc += ask_levels_[static_cast<std::size_t>(p)].total;
                if (acc >= qty) return true;
                const std::size_t nx = ask_occ_.next_at_or_above(static_cast<std::size_t>(p) + 1);
                if (nx == Occupancy::npos) break;
                p = static_cast<Price>(nx);
            }
        } else {
            for (Price p = best_bid_; p >= 0 && p >= limit;) {
                acc += bid_levels_[static_cast<std::size_t>(p)].total;
                if (acc >= qty) return true;
                if (p == 0) break;
                const std::size_t px = bid_occ_.prev_at_or_below(static_cast<std::size_t>(p) - 1);
                if (px == Occupancy::npos) break;
                p = static_cast<Price>(px);
            }
        }
        return false;
    }

    // Cancel a resting order by id. Returns true if it existed and was removed.
    bool cancel(OrderId id) {
        const std::uint32_t slot = id_map_.get(id);
        if (slot == FlatIdMap::NIL) return false;
        const Side side = pool_[slot].side;
        const Price price = pool_[slot].price;
        auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
        unlink(levels, slot);
        release(slot);
        id_map_.erase(id);
        if (levels[static_cast<std::size_t>(price)].head == NIL) {
            if (side == Side::Buy) {
                bid_occ_.clear(static_cast<std::size_t>(price));
                if (price == best_bid_) refresh_best_bid(price - 1);
            } else {
                ask_occ_.clear(static_cast<std::size_t>(price));
                if (price == best_ask_) refresh_best_ask(price + 1);
            }
        }
        return true;
    }

    bool has_bid() const { return best_bid_ >= 0; }
    bool has_ask() const { return best_ask_ < num_ticks_; }
    Price best_bid() const { return best_bid_; }
    Price best_ask() const { return best_ask_; }
    // Meaningful only when has_bid() && has_ask(); otherwise it mixes the sentinels.
    Price spread() const { return best_ask_ - best_bid_; }
    Price num_ticks() const { return num_ticks_; }
    std::size_t resting_orders() const { return id_map_.size(); }

    // Aggregate resting quantity at a price level (0 if none / out of range).
    // 64-bit: a level's total can exceed one order's 32-bit Qty range.
    std::uint64_t qty_at(Side side, Price price) const {
        if (price < 0 || price >= num_ticks_) return 0;
        return (side == Side::Buy ? bid_levels_ : ask_levels_)[static_cast<std::size_t>(price)].total;
    }

    // Per-order walk of one price level in FIFO (time-priority) order, calling
    // f(order_id, remaining_qty). O(orders at the level); for market data/diagnostics.
    template <typename F>
    void for_orders(Side side, Price price, F&& f) const {
        if (price < 0 || price >= num_ticks_) return;
        const auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
        for (std::uint32_t s = levels[static_cast<std::size_t>(price)].head; s != NIL;
             s = pool_[s].next)
            f(pool_[s].id, pool_[s].qty);
    }

    // L2 snapshot: visit up to `depth` populated levels from the best inward,
    // calling f(price, aggregate_qty). O(depth) via the occupancy bitmap.
    template <typename F>
    void for_levels(Side side, int depth, F&& f) const {
        if (side == Side::Buy) {
            for (Price p = best_bid_; depth > 0 && p >= 0; --depth) {
                f(p, bid_levels_[static_cast<std::size_t>(p)].total);
                if (p == 0) break;
                const std::size_t px = bid_occ_.prev_at_or_below(static_cast<std::size_t>(p) - 1);
                if (px == Occupancy::npos) break;
                p = static_cast<Price>(px);
            }
        } else {
            for (Price p = best_ask_; depth > 0 && p < num_ticks_; --depth) {
                f(p, ask_levels_[static_cast<std::size_t>(p)].total);
                const std::size_t nx = ask_occ_.next_at_or_above(static_cast<std::size_t>(p) + 1);
                if (nx == Occupancy::npos) break;
                p = static_cast<Price>(nx);
            }
        }
    }

private:
    static constexpr std::uint32_t NIL = 0xFFFFFFFFu;

    struct Node {
        OrderId id;
        Price price;
        Qty qty;
        std::uint32_t next;  // toward tail (younger)
        std::uint32_t prev;  // toward head (older)
        Side side;
    };
    struct Level {
        std::uint32_t head = NIL;  // oldest resting order
        std::uint32_t tail = NIL;  // newest
        // 64-bit on purpose: many u32-sized orders at one price overflow a u32 total,
        // silently corrupting qty_at/can_fill and making FOK falsely kill fillable
        // orders. The extra 4 bytes (Level: 12 -> 16, still 4/cache line) buy exactness.
        std::uint64_t total = 0;
    };

    std::uint32_t alloc(OrderId id, Price price, Qty qty, Side side) {
        std::uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
            pool_[slot] = Node{id, price, qty, NIL, NIL, side};
        } else {
            assert(pool_.size() < NIL);  // slot NIL aliases the id-map's empty sentinel
            slot = static_cast<std::uint32_t>(pool_.size());
            pool_.push_back(Node{id, price, qty, NIL, NIL, side});
        }
        return slot;
    }
    void release(std::uint32_t slot) { free_.push_back(slot); }

    void link_back(Level& lv, std::uint32_t slot) {
        Node& n = pool_[slot];
        n.prev = lv.tail;
        n.next = NIL;
        if (lv.tail != NIL) pool_[lv.tail].next = slot;
        else lv.head = slot;
        lv.tail = slot;
        lv.total += n.qty;
    }

    void unlink(std::vector<Level>& levels, std::uint32_t slot) {
        Node& n = pool_[slot];
        Level& lv = levels[static_cast<std::size_t>(n.price)];
        if (n.prev != NIL) pool_[n.prev].next = n.next;
        else lv.head = n.next;
        if (n.next != NIL) pool_[n.next].prev = n.prev;
        else lv.tail = n.prev;
        lv.total -= n.qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const std::uint32_t slot = alloc(id, price, qty, side);
        const std::size_t p = static_cast<std::size_t>(price);
        if (side == Side::Buy) {
            Level& lv = bid_levels_[p];
            const bool was_empty = (lv.head == NIL);
            link_back(lv, slot);
            if (was_empty) bid_occ_.set(p);  // only the first order at a level flips the bit
            if (price > best_bid_) best_bid_ = price;
        } else {
            Level& lv = ask_levels_[p];
            const bool was_empty = (lv.head == NIL);
            link_back(lv, slot);
            if (was_empty) ask_occ_.set(p);
            if (price < best_ask_) best_ask_ = price;
        }
        id_map_.set(id, slot);
    }

    // best_ask_ := lowest occupied ask tick >= `from` (num_ticks_ if none).
    void refresh_best_ask(Price from) {
        const std::size_t r = ask_occ_.next_at_or_above(static_cast<std::size_t>(from < 0 ? 0 : from));
        best_ask_ = (r == Occupancy::npos) ? num_ticks_ : static_cast<Price>(r);
    }
    // best_bid_ := highest occupied bid tick <= `from` (-1 if none / from < 0).
    void refresh_best_bid(Price from) {
        if (from < 0) { best_bid_ = -1; return; }
        const std::size_t r = bid_occ_.prev_at_or_below(static_cast<std::size_t>(from));
        best_bid_ = (r == Occupancy::npos) ? -1 : static_cast<Price>(r);
    }

    // Match a taker against the opposite side up to its price limit. An IsBuy taker
    // lifts asks with price <= limit; a seller hits bids with price >= limit.
    template <bool IsBuy>
    Qty match(OrderId taker, Price limit, Qty qty, std::vector<Trade>& out) {
        auto& levels = IsBuy ? ask_levels_ : bid_levels_;
        while (qty > 0) {
            if constexpr (IsBuy) {
                if (best_ask_ >= num_ticks_ || best_ask_ > limit) break;
            } else {
                if (best_bid_ < 0 || best_bid_ < limit) break;
            }
            const Price at = IsBuy ? best_ask_ : best_bid_;
            Level& lv = levels[static_cast<std::size_t>(at)];
            while (qty > 0 && lv.head != NIL) {
                const std::uint32_t hs = lv.head;
                Node& mk = pool_[hs];
                const Qty t = qty < mk.qty ? qty : mk.qty;
                out.push_back(Trade{taker, mk.id, at, t});
                qty -= t;
                mk.qty -= t;
                lv.total -= t;
                if (mk.qty == 0) {
                    lv.head = mk.next;
                    if (lv.head != NIL) pool_[lv.head].prev = NIL;
                    else lv.tail = NIL;
                    id_map_.erase(mk.id);
                    release(hs);
                }
            }
            if (lv.head == NIL) {
                if constexpr (IsBuy) {
                    ask_occ_.clear(static_cast<std::size_t>(at));
                    refresh_best_ask(at + 1);
                } else {
                    bid_occ_.clear(static_cast<std::size_t>(at));
                    refresh_best_bid(at - 1);
                }
            }
        }
        return qty;
    }

    Price num_ticks_;
    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    Occupancy bid_occ_;
    Occupancy ask_occ_;
    Price best_bid_;
    Price best_ask_;
    std::vector<Node> pool_;
    std::vector<std::uint32_t> free_;
    FlatIdMap id_map_;
};

}  // namespace ob
