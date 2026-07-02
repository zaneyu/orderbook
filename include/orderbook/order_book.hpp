#pragma once
//
// A price-time-priority limit order book / matching engine.
//
// Design: an array-indexed price ladder. Prices are integer ticks in [0, num_ticks).
// Each side keeps a vector<Level> indexed by tick; each Level is an intrusive FIFO
// (doubly linked list) of order nodes drawn from a reused pool. Best bid / best ask
// are cached tick indices.
//
//   add_limit  : O(1) to rest; matching is O(fills)
//   cancel     : O(1) unlink (+ amortized best-pointer walk when a best level empties)
//   match      : O(1) per fill
//
// The best-pointer walk on an emptied best level is O(ticks) worst case but amortized
// away in practice (the pointer only moves as levels are consumed). All hot paths are
// allocation-free after warm-up: nodes come from a free list, not new/delete.
//
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

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
          best_bid_(-1),
          best_ask_(num_ticks) {
        if (reserve_orders) {
            pool_.reserve(reserve_orders);
            free_.reserve(reserve_orders);
            id_map_.reserve(reserve_orders);
        }
    }

    // Add a limit order. Any quantity that crosses the book matches immediately at
    // resting prices (price-time priority); the remainder rests. Fills are appended
    // to `out`. Returns the resting (unfilled) quantity. A duplicate/known id or an
    // out-of-range/zero-qty order is rejected (returns 0, no fills, no state change).
    Qty add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_ || id_map_.count(id)) return 0;
        Qty rem = (side == Side::Buy) ? match<true>(id, price, qty, out)
                                      : match<false>(id, price, qty, out);
        if (rem > 0) rest(id, side, price, rem);
        return rem;
    }

    // Market order: match up to `qty` against the opposite side; never rests. Returns
    // the unfilled quantity (nonzero only if the book ran dry).
    Qty add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& out) {
        if (qty == 0) return 0;
        return (side == Side::Buy) ? match<true>(id, num_ticks_ - 1, qty, out)
                                   : match<false>(id, 0, qty, out);
    }

    // Cancel a resting order by id. Returns true if it existed and was removed.
    bool cancel(OrderId id) {
        auto it = id_map_.find(id);
        if (it == id_map_.end()) return false;
        const std::uint32_t slot = it->second;
        const Node& n = pool_[slot];
        const Side side = n.side;
        const Price price = n.price;
        unlink(side == Side::Buy ? bid_levels_ : ask_levels_, slot);
        release(slot);
        id_map_.erase(it);
        fix_best_after_removal(side, price);
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

    // Append an already-allocated node to the tail of its price level's FIFO.
    void link_back(Level& lv, std::uint32_t slot) {
        Node& n = pool_[slot];
        n.prev = lv.tail;
        n.next = NIL;
        if (lv.tail != NIL) pool_[lv.tail].next = slot;
        else lv.head = slot;
        lv.tail = slot;
        lv.total += n.qty;
    }

    // Remove an arbitrary node from its price level's FIFO.
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
        if (side == Side::Buy) {
            link_back(bid_levels_[static_cast<std::size_t>(price)], slot);
            if (price > best_bid_) best_bid_ = price;
        } else {
            link_back(ask_levels_[static_cast<std::size_t>(price)], slot);
            if (price < best_ask_) best_ask_ = price;
        }
        id_map_.emplace(id, slot);
    }

    // Match a taker against the opposite side up to its price limit. IsBuy taker
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
            if (lv.head == NIL) advance_best<IsBuy>();
        }
        return qty;
    }

    // Move the best pointer off an emptied best level to the next non-empty one.
    template <bool IsBuy>
    void advance_best() {
        if constexpr (IsBuy) {
            do { ++best_ask_; } while (best_ask_ < num_ticks_ && ask_levels_[static_cast<std::size_t>(best_ask_)].head == NIL);
        } else {
            do { --best_bid_; } while (best_bid_ >= 0 && bid_levels_[static_cast<std::size_t>(best_bid_)].head == NIL);
        }
    }

    // After a cancel, if the emptied level was the best, walk to the next non-empty.
    void fix_best_after_removal(Side side, Price price) {
        if (side == Side::Buy) {
            if (price == best_bid_ && bid_levels_[static_cast<std::size_t>(price)].head == NIL)
                while (best_bid_ >= 0 && bid_levels_[static_cast<std::size_t>(best_bid_)].head == NIL) --best_bid_;
        } else {
            if (price == best_ask_ && ask_levels_[static_cast<std::size_t>(price)].head == NIL)
                while (best_ask_ < num_ticks_ && ask_levels_[static_cast<std::size_t>(best_ask_)].head == NIL) ++best_ask_;
        }
    }

    Price num_ticks_;
    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    Price best_bid_;
    Price best_ask_;
    std::vector<Node> pool_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<OrderId, std::uint32_t> id_map_;
};

}  // namespace ob
