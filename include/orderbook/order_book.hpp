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
// All hot paths are allocation-free once sized: order nodes come from a free list and
// the id->slot index is an open-addressing flat map (flat_id_map.hpp), not a node-based
// std::unordered_map. tests/test_alloc_audit.cpp proves zero new/delete on the hot path.
//
#include <cstddef>
#include <cstdint>
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
        : num_ticks_(num_ticks),
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
    Price spread() const { return best_ask_ - best_bid_; }
    Price num_ticks() const { return num_ticks_; }
    std::size_t resting_orders() const { return id_map_.size(); }

    // Aggregate resting quantity at a price level (0 if none / out of range).
    Qty qty_at(Side side, Price price) const {
        if (price < 0 || price >= num_ticks_) return 0;
        return (side == Side::Buy ? bid_levels_ : ask_levels_)[static_cast<std::size_t>(price)].total;
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
        Qty total = 0;
    };

    std::uint32_t alloc(OrderId id, Price price, Qty qty, Side side) {
        std::uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
            pool_[slot] = Node{id, price, qty, NIL, NIL, side};
        } else {
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
