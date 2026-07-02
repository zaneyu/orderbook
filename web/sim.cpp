// WebAssembly bindings: the real matching engine, plus a small market simulator so
// the book is alive, exposed to JS via Embind. Compiled with emcc (see build.sh).
// Nothing here reimplements matching — it drives the same ob::OrderBook the C++
// tests and benchmarks use.
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "orderbook/order_book.hpp"

using namespace ob;
using emscripten::val;

namespace {
constexpr std::size_t RESERVE = 1u << 16;  // pool/id-map capacity (live orders are capped well below)
}

class Sim {
public:
    explicit Sim(int ticks)
        : ticks_(ticks), book_(ticks, RESERVE), rng_(0x9E3779B9ull), fv_(ticks / 2) {
        seed();
    }

    void reset() {
        book_ = OrderBook(ticks_, RESERVE);
        live_.clear();
        tape_.clear();
        next_id_ = 1;
        trades_ = 0;
        last_ = -1;
        fv_ = ticks_ / 2;
        seed();
    }

    // Manual order. type: 0 limit, 1 market, 2 ioc, 3 fok. side: 0 buy, 1 sell.
    val submit(int type, int side, int price, int qty) {
        fills_.clear();
        const Side s = side == 0 ? Side::Buy : Side::Sell;
        const OrderId id = next_id_++;
        Qty rem = static_cast<Qty>(qty);
        switch (type) {
            case 1: rem = book_.add_market(id, s, static_cast<Qty>(qty), fills_); break;
            case 2: rem = book_.add_ioc(id, s, price, static_cast<Qty>(qty), fills_); break;
            case 3: rem = book_.add_fok(id, s, price, static_cast<Qty>(qty), fills_); break;
            default:
                rem = book_.add_limit(id, s, price, static_cast<Qty>(qty), fills_);
                if (rem > 0) live_.push_back(id);
                break;
        }
        record();
        val r = val::object();
        r.set("filled", qty - static_cast<int>(rem));
        r.set("resting", static_cast<int>(rem));
        return r;
    }

    void step(int n) {
        for (int i = 0; i < n; ++i) one_step();
    }

    // Full view for one render frame: L2 ladder + stats + recent tape.
    val snapshot(int depth) {
        val o = val::object();
        o.set("bids", side_levels(Side::Buy, depth));
        o.set("asks", side_levels(Side::Sell, depth));
        o.set("bestBid", book_.has_bid() ? static_cast<int>(book_.best_bid()) : -1);
        o.set("bestAsk", book_.has_ask() ? static_cast<int>(book_.best_ask()) : -1);
        o.set("lastTrade", static_cast<int>(last_));
        o.set("trades", static_cast<double>(trades_));
        o.set("resting", static_cast<double>(book_.resting_orders()));
        o.set("ticks", ticks_);
        val t = val::array();
        int i = 0;
        for (const auto& tr : tape_) {
            val e = val::object();
            e.set("price", static_cast<int>(tr.price));
            e.set("qty", static_cast<double>(tr.qty));
            t.call<void>("push", e);
            if (++i >= 30) break;
        }
        o.set("tape", t);
        return o;
    }

private:
    val side_levels(Side side, int depth) {
        val arr = val::array();
        book_.for_levels(side, depth, [&](Price p, Qty q) {
            val lvl = val::array();
            lvl.call<void>("push", static_cast<int>(p));
            lvl.call<void>("push", static_cast<double>(q));
            arr.call<void>("push", lvl);
        });
        return arr;
    }

    void record() {
        for (const auto& f : fills_) {
            last_ = f.price;
            ++trades_;
            tape_.push_front(f);
        }
        while (tape_.size() > 60) tape_.pop_back();
    }

    void one_step() {
        fills_.clear();
        if ((rng_() & 7) == 0) fv_ += (rng_() & 1) ? 1 : -1;  // slow random walk
        const Price lo = 5, hi = ticks_ - 6;
        if (fv_ < lo) fv_ = lo;
        if (fv_ > hi) fv_ = hi;

        const int roll = static_cast<int>(rng_() % 100);
        const OrderId id = next_id_++;
        if (roll < 46) {  // rest a limit near the touch (provides depth)
            const bool buy = rng_() & 1;
            const Price off = static_cast<Price>(1 + rng_() % 4);
            const Price px = buy ? fv_ - off : fv_ + off;
            const Qty q = static_cast<Qty>(1 + rng_() % 25);
            if (book_.add_limit(id, buy ? Side::Buy : Side::Sell, px, q, fills_) > 0) live_.push_back(id);
        } else if (roll < 72) {  // marketable order -> trades print
            const bool buy = rng_() & 1;
            book_.add_market(id, buy ? Side::Buy : Side::Sell, static_cast<Qty>(1 + rng_() % 14), fills_);
        } else if (!live_.empty()) {  // cancel a random resting order
            const std::size_t k = rng_() % live_.size();
            book_.cancel(live_[k]);
            live_[k] = live_.back();
            live_.pop_back();
        }
        if (live_.size() > 4000) {  // keep the book bounded
            book_.cancel(live_.front());
            live_.erase(live_.begin());
        }
        record();
    }

    void seed() {
        for (int i = 0; i < 400; ++i) one_step();
    }

    int ticks_;
    OrderBook book_;
    std::mt19937_64 rng_;
    Price fv_;
    std::vector<Trade> fills_;
    std::deque<Trade> tape_;
    std::vector<OrderId> live_;
    OrderId next_id_ = 1;
    std::uint64_t trades_ = 0;
    Price last_ = -1;
};

EMSCRIPTEN_BINDINGS(orderbook) {
    emscripten::class_<Sim>("Sim")
        .constructor<int>()
        .function("reset", &Sim::reset)
        .function("submit", &Sim::submit)
        .function("step", &Sim::step)
        .function("snapshot", &Sim::snapshot);
}
