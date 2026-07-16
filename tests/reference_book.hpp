#pragma once
//
// A deliberately naive, obviously-correct order book used as the oracle in the
// differential fuzz test. Same public API and price-time semantics as the fast
// OrderBook, implemented with std::map + std::deque so there is nothing clever to
// get wrong. If the fast book ever diverges from this on any random op stream, the
// fast book is buggy.
//
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <utility>
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

    Qty add_ioc(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_) return qty;
        if (side == Side::Buy)
            while (qty > 0 && !asks_.empty() && asks_.begin()->first <= price)
                qty = fill_front(asks_, asks_.begin()->first, id, qty, out);
        else
            while (qty > 0 && !bids_.empty() && bids_.rbegin()->first >= price)
                qty = fill_front(bids_, bids_.rbegin()->first, id, qty, out);
        return qty;  // never rests
    }

    Qty add_fok(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || price < 0 || price >= num_ticks_) return qty;
        std::uint64_t acc = 0;
        if (side == Side::Buy) {
            for (const auto& kv : asks_) {
                if (kv.first > price) break;
                for (const auto& r : kv.second) acc += r.qty;
                if (acc >= qty) break;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (it->first < price) break;
                for (const auto& r : it->second) acc += r.qty;
                if (acc >= qty) break;
            }
        }
        if (acc < qty) return qty;                 // kill
        return add_ioc(id, side, price, qty, out);  // fully fills
    }

    bool modify(OrderId id, Price new_price, Qty new_qty, std::vector<Trade>& out) {
        for (int s = 0; s < 2; ++s) {
            Book& book = (s == 0) ? bids_ : asks_;
            const Side side = (s == 0) ? Side::Buy : Side::Sell;
            for (auto it = book.begin(); it != book.end(); ++it) {
                for (auto jt = it->second.begin(); jt != it->second.end(); ++jt) {
                    if (jt->id != id) continue;
                    if (new_qty != 0 && new_price == it->first && new_qty <= jt->qty) {
                        jt->qty = new_qty;  // in-place, keeps priority
                        return true;
                    }
                    if (new_qty == 0) {  // documented: new_qty == 0 cancels
                        it->second.erase(jt);
                        if (it->second.empty()) book.erase(it);
                        ids_.erase(id);
                        return true;
                    }
                    // Contract: an out-of-range target price rejects the modify and
                    // leaves the order untouched — checked BEFORE erasing. (This oracle
                    // used to erase first and let add_limit reject, silently destroying
                    // the order the fast book correctly preserves.)
                    if (new_price < 0 || new_price >= num_ticks_) return false;
                    it->second.erase(jt);
                    if (it->second.empty()) book.erase(it);
                    ids_.erase(id);
                    add_limit(id, side, new_price, new_qty, out);
                    return true;
                }
            }
        }
        return false;
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

    std::uint64_t qty_at(Side side, Price price) const {
        const auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto it = book.find(price);
        if (it == book.end()) return 0;
        std::uint64_t s = 0;  // 64-bit: a level's total can exceed one order's Qty range
        for (const auto& o : it->second) s += o.qty;
        return s;
    }

    // Per-order FIFO at one level (oldest first), for deep differential comparison.
    std::vector<std::pair<OrderId, Qty>> orders_at(Side side, Price price) const {
        std::vector<std::pair<OrderId, Qty>> v;
        const auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto it = book.find(price);
        if (it != book.end())
            for (const auto& o : it->second) v.emplace_back(o.id, o.qty);
        return v;
    }

    std::size_t resting_orders() const { return ids_.size(); }

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
