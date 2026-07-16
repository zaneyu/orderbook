#pragma once
//
// Builds real order books from a NASDAQ TotalView-ITCH 5.0 stream, one ob::OrderBook
// per tracked symbol, and VALIDATES the engine against the exchange's own events:
//
//  - adds use OrderBook::rest_only (the venue already matched; executions arrive as
//    separate E/C events), so legitimately crossed pre-open books replay exactly
//  - E/C (executed) and X (partial cancel) become in-place decreases — the same
//    modify path the engine exposes — keeping queue priority, per the ITCH spec
//  - U (replace) is delete + re-add under the new ref: priority lost, per spec
//  - FRONT-OF-QUEUE FIDELITY: before applying each regular-hours execution, we check
//    that the executed order is exactly the order OUR book has at the front of the
//    FIFO at the best price. The exchange matched price-time against its book; if
//    our book agrees order-for-order, our FIFO and best-price tracking are right —
//    validated by the venue itself, millions of times per day.
//
// Prices: ITCH prices are u32 in 1/10000 dollars. Displayed NASDAQ orders >= $1 are
// penny-aligned; we build at penny ticks (price4/100). Sub-penny or out-of-ladder
// prices are counted and that order is ignored (its later events too).
//
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "itch.hpp"
#include "orderbook/order_book.hpp"

namespace itch {

struct Event {
    enum Kind : std::uint8_t {
        Add,     // order rested (A/F, or the re-add half of U)
        Exec,    // shares traded out of a displayed order (E/C)
        Reduce,  // shares cancelled off, priority kept (X)
        Remove,  // order fully removed NOT via execution (D, or the delete half of U)
    } kind;
    std::uint16_t book;  // index into BookBuilder's tracked books
    ob::Side side;
    ob::Price price;     // tick
    ob::OrderId ref;
    std::uint32_t shares;  // delta (Remove: the remaining shares that left)
    std::uint64_t ts;      // nanos since midnight
    // Exec only: true for 'E' (traded AT the display price), false for 'C' (traded at
    // some other price — cross/price-improved path). Both drain the queue; only
    // display-price volume evidences trading interest at that level.
    bool at_display = true;
};

class BookBuilder {
public:
    struct Config {
        std::vector<std::string> symbols;      // e.g. {"AAPL", "MSFT"}
        ob::Price num_ticks = 1 << 18;         // pennies: covers $0 .. $2621.43
        std::size_t reserve_orders = 1u << 20; // per-book node/id-map reservation
    };

    struct Sink {  // queue-position simulator hooks in here
        virtual void on_event(const Event&) {}
        virtual ~Sink() = default;
    };

    struct Stats {
        std::uint64_t msgs = 0;             // every message seen
        std::uint64_t malformed = 0;        // length mismatch for a known type
        std::uint64_t adds = 0;             // applied to tracked books
        std::uint64_t execs = 0;            // E/C applied
        std::uint64_t cancels = 0;          // X applied
        std::uint64_t deletes = 0;          // D applied
        std::uint64_t replaces = 0;         // U applied
        std::uint64_t hidden_trades = 0;    // P on tracked symbols (no book effect)
        std::uint64_t unknown_ref = 0;      // E/C/X/D/U referencing an untracked ref
        std::uint64_t subpenny_skipped = 0; // displayed order not penny-aligned
        std::uint64_t out_of_band = 0;      // price beyond the ladder
        std::uint64_t ignored_events = 0;   // events on deliberately-skipped orders
        std::uint64_t qty_underflow = 0;    // exec/cancel larger than tracked remainder
        std::uint64_t crossing_adds_rth = 0;// adds crossing our book in regular hours
        std::uint64_t rest_rejected = 0;    // rest_only refused (should stay 0)
        // Front-of-queue fidelity, regular hours, uncrossed book only. Reported in two
        // buckets because the first minutes after the open contain opening-process
        // RELEASES: on-open interest entering the continuous book whose A message
        // appears at ~9:30:0x but whose exchange-internal priority predates it (their
        // old-vintage order refs give it away). Public ITCH cannot reconstruct that
        // rank, so the exchange legitimately fills "behind" our head there. Steady
        // state (after the open window) is where the engine's price-time FIFO is
        // genuinely testable against the venue.
        std::uint64_t e_rth = 0;            // executions checked (all RTH)
        std::uint64_t e_front_best = 0;     // hit the head of OUR best-price FIFO
        std::uint64_t e_front_only = 0;     // head of its level, but not our best price
        std::uint64_t e_miss_second = 0;    // missed: ref was SECOND in our queue
        std::uint64_t e_miss_deeper = 0;    // missed: ref deeper (or level mismatch)
        std::uint64_t e_crossed = 0;        // exec while our book was crossed (excluded)
        std::uint64_t e_steady = 0;         // subset of e_rth after the open window
        std::uint64_t e_steady_front_best = 0;
        // The strictest bucket: steady-state executions at levels containing NO
        // released/re-displayed order (an add whose ref sits below the feed's global
        // ref watermark — opening-process releases and price-slid re-displays whose
        // exchange priority follows their original ENTRY, which is not public). On
        // these clean levels, price-time priority is fully reconstructible from the
        // feed, so this is the honest test of the engine's FIFO.
        std::uint64_t released_adds = 0;    // out-of-sequence adds detected
        std::uint64_t e_clean = 0;
        std::uint64_t e_clean_front_best = 0;
        std::uint64_t c_execs = 0;          // 'C': executed at a non-display price
    };

    static constexpr std::uint64_t kOpenWindowNs = 5ull * 60 * 1000 * 1000 * 1000;  // 5 min

    // The sink may need a pointer to this builder (e.g. QueueSim snapshots queues),
    // so it can also be attached after construction via set_sink().
    explicit BookBuilder(Config cfg, Sink* sink = nullptr)
        : cfg_(std::move(cfg)), sink_(sink), locate_to_book_(1 << 16, -1) {
        books_.reserve(cfg_.symbols.size());
        names_.reserve(cfg_.symbols.size());
        for (const auto& s : cfg_.symbols) {
            books_.emplace_back(cfg_.num_ticks, cfg_.reserve_orders);
            names_.push_back(s);
        }
        ref_watermark_.assign(cfg_.symbols.size(), 0);
        orders_.reserve(cfg_.reserve_orders);
    }

    void apply(const MsgView& m) {
        ++stats_.msgs;
        const char t = m.type();
        const std::size_t want = expected_len(t);
        if (want != 0 && m.len != want) {
            ++stats_.malformed;
            return;
        }
        switch (t) {
            case 'S': {
                const auto s = decode_system(m.p);
                ts_ = s.ts;
                if (s.code == 'Q') {  // start of market hours
                    market_open_ = true;
                    open_ts_ = s.ts;
                }
                if (s.code == 'M') market_open_ = false;  // end of market hours
                break;
            }
            case 'R': {
                const auto d = decode_directory(m.p);
                const int idx = symbol_index(d.stock);
                if (idx >= 0) locate_to_book_[d.locate] = static_cast<std::int32_t>(idx);
                break;
            }
            case 'A':
            case 'F': {
                const auto a = decode_add(m.p);
                ts_ = a.ts;
                const std::int32_t bi = locate_to_book_[a.locate];
                if (bi < 0) break;
                on_add(static_cast<std::uint16_t>(bi), a.ref, a.side, a.shares, a.price4, a.ts);
                break;
            }
            case 'E':
            case 'C': {
                const auto e = decode_exec(m.p);
                ts_ = e.ts;
                if (locate_to_book_[e.locate] < 0) break;  // untracked symbol
                // 'C' = executed at a price OTHER than the display price (crosses,
                // price-improved fills): a different matching path that does not
                // follow display-price queue order, so it updates the book but is
                // excluded from the front-of-queue fidelity metric.
                on_reduce(e.ref, e.shares, e.ts, /*is_exec=*/true, /*at_display=*/t == 'E');
                break;
            }
            case 'X': {
                const auto x = decode_cancel(m.p);
                ts_ = x.ts;
                if (locate_to_book_[x.locate] < 0) break;
                on_reduce(x.ref, x.shares, x.ts, /*is_exec=*/false, /*at_display=*/false);
                break;
            }
            case 'D': {
                const auto d = decode_delete(m.p);
                ts_ = d.ts;
                if (locate_to_book_[d.locate] < 0) break;
                on_delete(d.ref, d.ts);
                break;
            }
            case 'U': {
                const auto u = decode_replace(m.p);
                ts_ = u.ts;
                if (locate_to_book_[u.locate] < 0) break;
                const auto it = orders_.find(u.orig_ref);
                if (it == orders_.end()) {
                    if (ignored_.count(u.orig_ref)) {
                        // replacing an order we deliberately skipped: we don't know its
                        // side, so the replacement can't be booked either — chain it
                        ++stats_.ignored_events;
                        ignored_.erase(u.orig_ref);
                        ignored_.insert(u.new_ref);
                        break;
                    }
                    // tracked symbol (checked above), but the orig ref was never seen:
                    // a genuine feed/book anomaly
                    ++stats_.unknown_ref;
                    break;
                }
                ++stats_.replaces;
                const OrderInfo orig = it->second;  // copy BEFORE on_delete invalidates `it`
                on_delete(u.orig_ref, u.ts);        // priority lost, per spec
                on_add(orig.book, u.new_ref, it_side_char(orig.side), u.shares, u.price4, u.ts);
                break;
            }
            case 'P': {
                const auto h = decode_hidden_trade(m.p);
                ts_ = h.ts;
                if (locate_to_book_[h.locate] >= 0) ++stats_.hidden_trades;
                break;
            }
            default:
                break;  // everything else is skippable by the length prefix
        }
    }

    void set_sink(Sink* sink) { sink_ = sink; }

    std::size_t num_books() const { return books_.size(); }
    const ob::OrderBook& book(std::size_t i) const { return books_[i]; }
    const std::string& name(std::size_t i) const { return names_[i]; }
    const Stats& stats() const { return stats_; }
    bool market_open() const { return market_open_; }
    std::uint64_t last_ts() const { return ts_; }
    std::size_t tracked_orders() const { return orders_.size(); }

    // Penny tick for an ITCH price4, or -1 if sub-penny / out of the ladder.
    ob::Price to_tick(std::uint32_t price4) const {
        if (price4 % 100 != 0) return -1;
        const std::uint32_t t = price4 / 100;
        if (t >= static_cast<std::uint32_t>(cfg_.num_ticks)) return -1;
        return static_cast<ob::Price>(t);
    }

private:
    struct OrderInfo {
        std::uint16_t book;
        ob::Side side;
        ob::Price price;
        std::uint32_t qty;
        bool released;  // out-of-sequence add: exchange priority not public
    };

    // Key one price level of one book+side for the released-order taint count.
    static std::uint64_t level_key(std::uint16_t book, ob::Side side, ob::Price price) {
        return (std::uint64_t{book} << 40) |
               (std::uint64_t{side == ob::Side::Buy ? 1u : 0u} << 32) |
               static_cast<std::uint32_t>(price);
    }

    static char it_side_char(ob::Side s) { return s == ob::Side::Buy ? 'B' : 'S'; }

    int symbol_index(const char* wire8) const {  // wire is space-padded to 8
        std::size_t n = 8;
        while (n > 0 && (wire8[n - 1] == ' ' || wire8[n - 1] == '\0')) --n;
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i].size() == n && std::memcmp(names_[i].data(), wire8, n) == 0)
                return static_cast<int>(i);
        return -1;
    }

    void on_add(std::uint16_t bi, ob::OrderId ref, char side_c, std::uint32_t shares,
                std::uint32_t price4, std::uint64_t ts) {
        const ob::Price tick = to_tick(price4);
        if (tick < 0) {
            ++(price4 % 100 != 0 ? stats_.subpenny_skipped : stats_.out_of_band);
            ignored_.insert(ref);  // its later E/X/D/U must not read as unknown refs
            return;
        }
        const ob::Side side = (side_c == 'B') ? ob::Side::Buy : ob::Side::Sell;
        ob::OrderBook& bk = books_[bi];
        // Crossing adds are impossible in continuous trading if our book is right —
        // the exchange would have executed the incoming order instead. Pre-open they
        // are legitimate (matching is suspended until the opening cross).
        if (market_open_) {
            const bool crosses = (side == ob::Side::Buy)
                                     ? (bk.has_ask() && tick >= bk.best_ask())
                                     : (bk.has_bid() && tick <= bk.best_bid());
            if (crosses) ++stats_.crossing_adds_rth;
        }
        if (!bk.rest_only(ref, side, tick, shares)) {
            ++stats_.rest_rejected;  // duplicate ref — should never happen in a feed
            return;
        }
        // Ref numbers are assigned at ENTRY and, within one symbol's stream, appear in
        // increasing order during continuous trading (symbols are partitioned across
        // matching engines, so the watermark must be per book — a global one misfires
        // on cross-partition interleaving). An add below its book's watermark is a
        // release/re-display whose true queue rank is not public — taint its level
        // for the fidelity metric. Over-tainting only shrinks the clean sample; it
        // can never fake fidelity.
        const bool released = ref < ref_watermark_[bi];
        if (ref > ref_watermark_[bi]) ref_watermark_[bi] = ref;
        if (released) {
            ++stats_.released_adds;
            ++released_at_level_[level_key(bi, side, tick)];
        }
        orders_.emplace(ref, OrderInfo{bi, side, tick, shares, released});
        ++stats_.adds;
        if (sink_) sink_->on_event({Event::Add, bi, side, tick, ref, shares, ts});
    }

    void untaint_if_released(const OrderInfo& o, ob::OrderId /*ref*/) {
        if (!o.released) return;
        const auto it = released_at_level_.find(level_key(o.book, o.side, o.price));
        if (it != released_at_level_.end() && --it->second == 0) released_at_level_.erase(it);
    }

    void on_reduce(ob::OrderId ref, std::uint32_t shares, std::uint64_t ts, bool is_exec,
                   bool at_display) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) {
            if (ignored_.count(ref)) ++stats_.ignored_events;
            else ++stats_.unknown_ref;
            return;
        }
        OrderInfo& o = it->second;
        ob::OrderBook& bk = books_[o.book];
        if (is_exec) {
            ++stats_.execs;
            if (!at_display) ++stats_.c_execs;
            const bool crossed = bk.has_bid() && bk.has_ask() && bk.best_bid() >= bk.best_ask();
            if (market_open_ && crossed) {
                ++stats_.e_crossed;  // opening-cross unwind: not a continuous-trading exec
            } else if (market_open_ && at_display) {  // front-of-queue fidelity metric
                ++stats_.e_rth;
                const bool steady = ts >= open_ts_ + kOpenWindowNs;
                const bool clean =
                    steady && !released_at_level_.count(level_key(o.book, o.side, o.price));
                if (steady) ++stats_.e_steady;
                if (clean) ++stats_.e_clean;
                ob::OrderId head = 0;
                const bool at_front = bk.front_order(o.side, o.price, head) && head == ref;
                const bool at_best = (o.side == ob::Side::Buy)
                                         ? (bk.has_bid() && bk.best_bid() == o.price)
                                         : (bk.has_ask() && bk.best_ask() == o.price);
                if (at_front && at_best) {
                    ++stats_.e_front_best;
                    if (steady) ++stats_.e_steady_front_best;
                    if (clean) ++stats_.e_clean_front_best;
                } else if (at_front) {
                    ++stats_.e_front_only;
                } else {
                    // classify the miss: was the ref at least SECOND in our queue?
                    int pos = 0, refpos = -1;
                    bk.for_orders(o.side, o.price, [&](ob::OrderId id, ob::Qty) {
                        if (id == ref && refpos < 0) refpos = pos;
                        ++pos;
                    });
                    ++(refpos == 1 ? stats_.e_miss_second : stats_.e_miss_deeper);
#ifdef ITCH_DEBUG_MISSES
                    if (clean) {
                        ob::OrderId head2 = 0;
                        bk.front_order(o.side, o.price, head2);
                        static int dbg = 0;
                        if (dbg++ < 25)
                            std::fprintf(stderr,
                                         "CLEANMISS ts=%llu book=%u side=%c px=%d ref=%llu "
                                         "pos=%d head=%llu level_n=%d\n",
                                         (unsigned long long)ts, o.book,
                                         o.side == ob::Side::Buy ? 'B' : 'S', o.price,
                                         (unsigned long long)ref, refpos,
                                         (unsigned long long)head2, pos);
                    }
#endif
                }
            }
        } else {
            ++stats_.cancels;
        }
        std::uint32_t n = shares;
        if (n > o.qty) {  // feed says more left than we track: our state is wrong
            ++stats_.qty_underflow;
            n = o.qty;
        }
        const std::uint32_t left = o.qty - n;
        if (left == 0) {
            bk.cancel(ref);
            untaint_if_released(o, ref);
            // A full execution stays Exec (it IS traded volume); an X cancelling the
            // whole remainder is a removal-not-via-execution, i.e. Remove — sinks can
            // then drop the ref from ahead-sets exactly like a D.
            if (sink_)
                sink_->on_event({is_exec ? Event::Exec : Event::Remove, o.book, o.side, o.price,
                                 ref, n, ts, at_display});
            orders_.erase(it);
        } else {
            bk.modify(ref, o.price, left, scratch_);  // in-place decrease: priority kept,
            scratch_.clear();                          // and no fills can be emitted
            o.qty = left;
            if (sink_)
                sink_->on_event({is_exec ? Event::Exec : Event::Reduce, o.book, o.side, o.price,
                                 ref, n, ts, at_display});
        }
    }

    void on_delete(ob::OrderId ref, std::uint64_t ts) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) {
            if (ignored_.count(ref)) {
                ++stats_.ignored_events;
                ignored_.erase(ref);  // fully gone; keep the set from growing all day
            } else {
                ++stats_.unknown_ref;
            }
            return;
        }
        const OrderInfo o = it->second;
        books_[o.book].cancel(ref);
        untaint_if_released(o, ref);
        orders_.erase(it);
        ++stats_.deletes;
        if (sink_) sink_->on_event({Event::Remove, o.book, o.side, o.price, ref, o.qty, ts});
    }

    Config cfg_;
    Sink* sink_;
    std::vector<std::int32_t> locate_to_book_;  // locate code -> book index (-1 untracked)
    std::vector<ob::OrderBook> books_;
    std::vector<std::string> names_;
    std::unordered_map<ob::OrderId, OrderInfo> orders_;
    std::unordered_set<ob::OrderId> ignored_;  // skipped adds (sub-penny / out-of-ladder)
    std::unordered_map<std::uint64_t, std::uint32_t> released_at_level_;  // taint counts
    std::vector<ob::OrderId> ref_watermark_;  // per book: symbols span engine partitions
    std::vector<ob::Trade> scratch_;  // for modify(); in-place decreases never fill
    Stats stats_;
    bool market_open_ = false;
    std::uint64_t ts_ = 0;
    std::uint64_t open_ts_ = 0;
};

}  // namespace itch
