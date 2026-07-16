#pragma once
//
// Queue-position / fill-probability simulator on top of the ITCH book builder.
//
// At a fixed market-time cadence it inserts a PHANTOM passive order joining the back
// of the best-bid queue (configurable side) and tracks it against the real L3 flow —
// exactly, not statistically: at insertion it snapshots the set of real order refs
// ahead of it (the whole FIFO at that price), then
//
//   - executions/cancels/deletes of orders in the ahead-set reduce `ahead`
//   - once ahead == 0, further real executions at that price fill the phantom
//     (that volume actually traded at our price; we were in front of it)
//   - an execution printing STRICTLY through our price fills the remainder
//     (the market traded through the level; a resting order there doesn't survive)
//   - otherwise the phantom expires at the horizon with whatever filled
//
// The phantom is never inserted into the real book — the builder's state and its
// validation counters are untouched. One-sided assumption, stated plainly: our
// phantom does not change anyone else's behaviour (standard for this kind of study).
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book_builder.hpp"

namespace itch {

class QueueSim : public BookBuilder::Sink {
public:
    struct Config {
        std::uint32_t phantom_qty = 100;
        std::uint64_t sample_every_ns = 5ull * 1000 * 1000 * 1000;  // 5s market time
        std::uint64_t horizon_ns = 60ull * 1000 * 1000 * 1000;      // 60s
        ob::Side side = ob::Side::Buy;                              // join the bid queue
    };

    struct Trial {
        std::uint16_t book;
        ob::Price price;        // the level we joined
        std::uint64_t t0;       // insertion time (ns since midnight)
        std::uint64_t ahead0;   // shares ahead at insertion
        std::uint32_t norders0; // orders ahead at insertion
        ob::Price spread0;      // spread in ticks at insertion
        // live state
        std::uint64_t ahead;
        std::unordered_set<ob::OrderId> ahead_set;
        std::uint32_t remaining;
        std::uint64_t t_first = 0;  // first fill time (0 = none)
        std::uint64_t t_done = 0;   // fully filled / closed time
        std::uint32_t anomalies = 0;  // exec at our price behind us while ahead > 0
        bool traded_through = false;
        bool open = true;
    };

    QueueSim(Config cfg, const BookBuilder* builder) : cfg_(cfg), builder_(builder) {}

    void on_event(const Event& ev) override {
        auto& idx = by_book_[ev.book];
        for (std::size_t k = 0; k < idx.size();) {
            Trial& t = trials_[idx[k]];
            if (!t.open) {  // lazily drop closed trials from the per-book index
                idx[k] = idx.back();
                idx.pop_back();
                continue;
            }
            // Horizon first: an event arriving after expiry must not fill the phantom
            // (the order would already have been cancelled at the horizon).
            if (ev.ts > t.t0 && ev.ts - t.t0 > cfg_.horizon_ns) close_trial(t, t.t0 + cfg_.horizon_ns);
            else step_trial(t, ev);
            ++k;
        }
        // Sample LAST: a trial opened by this event snapshots the post-application
        // book, so stepping it on the same event would double-count the reduction
        // already reflected in ahead0 (measured: biased P(fill) optimistic by ~3%).
        maybe_sample(ev.ts);
    }

    // Close every still-open trial (end of stream). Clamped to each trial's horizon so
    // an `expired` row's t_close is horizon-consistent whether its book went quiet or
    // the stream simply ended.
    void flush(std::uint64_t ts) {
        for (auto& t : trials_)
            if (t.open) close_trial(t, std::min(ts, t.t0 + cfg_.horizon_ns));
    }

    const std::vector<Trial>& trials() const { return trials_; }

private:
    void maybe_sample(std::uint64_t ts) {
        if (!builder_->market_open()) return;
        if (next_sample_ == 0) next_sample_ = ts + cfg_.sample_every_ns;  // first open event
        if (ts < next_sample_) return;
        next_sample_ += cfg_.sample_every_ns;
        if (ts >= next_sample_) next_sample_ = ts + cfg_.sample_every_ns;  // gap: re-anchor
        for (std::uint16_t b = 0; b < static_cast<std::uint16_t>(builder_->num_books()); ++b)
            open_trial(b, ts);
    }

    void open_trial(std::uint16_t b, std::uint64_t ts) {
        const ob::OrderBook& bk = builder_->book(b);
        if (!bk.has_bid() || !bk.has_ask()) return;
        if (bk.spread() <= 0) return;  // locked/crossed at sampling: not a joinable queue
        Trial t;
        t.book = b;
        t.price = (cfg_.side == ob::Side::Buy) ? bk.best_bid() : bk.best_ask();
        t.t0 = ts;
        t.spread0 = bk.spread();
        std::uint64_t q = 0;
        std::uint32_t n = 0;
        auto& set = t.ahead_set;
        bk.for_orders(cfg_.side, t.price, [&](ob::OrderId id, ob::Qty qty) {
            set.insert(id);
            q += qty;
            ++n;
        });
        t.ahead0 = t.ahead = q;
        t.norders0 = n;
        t.remaining = cfg_.phantom_qty;
        by_book_[b].push_back(trials_.size());
        trials_.push_back(std::move(t));
    }

    void step_trial(Trial& t, const Event& ev) {
        // Trade-through: a display-price execution ON OUR SIDE strictly through our
        // price while we rest there means the market consumed the whole level — a real
        // order there would have filled. ('C' fills at non-display prices don't
        // evidence that, and an OPPOSITE-side order executing beyond our price — a
        // stale locked/crossed remnant — says nothing about our queue.)
        const bool through = (cfg_.side == ob::Side::Buy) ? (ev.price < t.price)
                                                          : (ev.price > t.price);
        if (ev.kind == Event::Exec && ev.at_display && ev.side == cfg_.side && through) {
            if (t.t_first == 0) t.t_first = ev.ts;
            t.remaining = 0;
            t.traded_through = true;
            close_trial(t, ev.ts);
            return;
        }
        if (ev.price != t.price || ev.side != cfg_.side) return;  // not our queue
        switch (ev.kind) {
            case Event::Exec:
                if (t.ahead_set.count(ev.ref)) {
                    // any execution of an ahead order drains the queue, E or C alike
                    t.ahead -= std::min<std::uint64_t>(t.ahead, ev.shares);
                } else if (!ev.at_display) {
                    // a 'C' behind us traded at some OTHER price — not volume at ours
                } else if (t.ahead == 0) {
                    // display-price volume trading with nobody left ahead: ours
                    const std::uint32_t take = std::min(t.remaining, ev.shares);
                    t.remaining -= take;
                    if (take > 0 && t.t_first == 0) t.t_first = ev.ts;
                    if (t.remaining == 0) close_trial(t, ev.ts);
                } else {
                    ++t.anomalies;  // price-time says this order was behind us
                }
                break;
            case Event::Reduce:
            case Event::Remove:
                if (t.ahead_set.count(ev.ref)) {
                    t.ahead -= std::min<std::uint64_t>(t.ahead, ev.shares);
                    if (ev.kind == Event::Remove) t.ahead_set.erase(ev.ref);
                }
                break;
            case Event::Add:
                break;  // joins behind the phantom: irrelevant to our fill
        }
    }

    void close_trial(Trial& t, std::uint64_t ts) {
        t.t_done = ts;
        t.open = false;
        t.ahead_set.clear();  // release memory; the per-book index drops it lazily
    }

    Config cfg_;
    const BookBuilder* builder_;
    std::vector<Trial> trials_;
    std::unordered_map<std::uint16_t, std::vector<std::size_t>> by_book_;
    std::uint64_t next_sample_ = 0;
};

}  // namespace itch
