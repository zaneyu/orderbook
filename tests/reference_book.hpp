#pragma once
//
// A deliberately naive, obviously-correct order book used as the oracle in the
// differential fuzz test. Same public API and price-time semantics as the fast
// OrderBook, implemented with std::map + std::deque so there is nothing clever to
// get wrong. If the fast book ever diverges from this on any random op stream, the
// fast book is buggy.
//
#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "orderbook/types.hpp"

namespace ob {

class ReferenceBook {
public:
    explicit ReferenceBook(Price num_ticks) : num_ticks_(num_ticks) {}

    Qty add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_ || ids_.count(id)) return 0;
        if (side == Side::Buy) {
            // lift asks from the lowest price while marketable
            while (qty > 0 && !asks_.empty() && asks_.begin()->first <= price)
                qty = fill_front(asks_, asks_.begin()->first, id, qty, out);
            if (qty > 0) { bids_[price].push_back({id, qty}); ids_.insert(id); }
        } else {
            while (qty > 0 && !bids_.empty() && bids_.rbegin()->first >= price)
                qty = fill_front(bids_, bids_.rbegin()->first, id, qty, out);
            if (qty > 0) { asks_[price].push_back({id, qty}); ids_.insert(id); }
        }
        return qty;
    }

    Qty add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& out) {
        if (qty == 0) return 0;
        if (side == Side::Buy)
            while (qty > 0 && !asks_.empty()) qty = fill_front(asks_, asks_.begin()->first, id, qty, out);
        else
            while (qty > 0 && !bids_.empty()) qty = fill_front(bids_, bids_.rbegin()->first, id, qty, out);
        return qty;
    }

    bool cancel(OrderId id) {
        if (!ids_.count(id)) return false;
        if (erase_from(bids_, id) || erase_from(asks_, id)) { ids_.erase(id); return true; }
        return false;
    }

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }
    Price best_bid() const { return bids_.rbegin()->first; }
    Price best_ask() const { return asks_.begin()->first; }

    Qty qty_at(Side side, Price price) const {
        const auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto it = book.find(price);
        if (it == book.end()) return 0;
        Qty s = 0;
        for (const auto& o : it->second) s += o.qty;
        return s;
    }

private:
    struct Resting { OrderId id; Qty qty; };
    using Book = std::map<Price, std::deque<Resting>>;

    // Fill the FIFO at `level` in `book` against a taker; erase the level if drained.
    Qty fill_front(Book& book, Price level, OrderId taker, Qty qty, std::vector<Trade>& out) {
        auto& q = book[level];
        while (qty > 0 && !q.empty()) {
            Resting& mk = q.front();
            const Qty t = qty < mk.qty ? qty : mk.qty;
            out.push_back(Trade{taker, mk.id, level, t});
            qty -= t;
            mk.qty -= t;
            if (mk.qty == 0) { ids_.erase(mk.id); q.pop_front(); }
        }
        if (q.empty()) book.erase(level);
        return qty;
    }

    static bool erase_from(Book& book, OrderId id) {
        for (auto it = book.begin(); it != book.end(); ++it) {
            auto& q = it->second;
            for (auto jt = q.begin(); jt != q.end(); ++jt) {
                if (jt->id == id) {
                    q.erase(jt);
                    if (q.empty()) book.erase(it);
                    return true;
                }
            }
        }
        return false;
    }

    Price num_ticks_;
    Book bids_;
    Book asks_;
    std::set<OrderId> ids_;
};

}  // namespace ob
