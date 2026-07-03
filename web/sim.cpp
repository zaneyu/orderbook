// WebAssembly bindings: the real matching engine, plus a market simulator so the
// book is alive, exposed to JS via Embind. Compiled with emcc (see build-wasm.sh).
// Nothing here reimplements matching — it drives the same ob::OrderBook the C++
// tests and benchmarks use. The *price process* is a small but genuine model:
// stochastic volatility (clustering), a persistent momentum/drift regime, fat-tailed
// news jumps, order flow that leans with the trend, and spreads that widen in
// turbulence. Your own aggressive orders exert market impact, so trading moves it.
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "orderbook/order_book.hpp"

using namespace ob;
using emscripten::val;

namespace {
constexpr std::size_t RESERVE = 1u << 16;  // pool/id-map capacity (live orders capped well below)
constexpr int kMaxQty = 1'000'000;         // hard cap on a single order (keeps level totals << 2^32)

// The price process is advanced by WALL-CLOCK time (a `dt` in 60fps-frame units), not
// per render frame or per order event — so price velocity is identical at any refresh
// rate, and the "speed" control changes market activity, not how fast the price moves.
// Units are ticks (1 tick = $0.01), rates per frame-equivalent. Calibrated for a ~$100
// instrument: lively but realistic (tens of cents/sec, trends lasting several seconds).
constexpr double kKappaVol = 0.02;    // mean-reversion speed of log-volatility
constexpr double kSigmaVol = 0.11;    // vol-of-vol -> volatility *clusters*
constexpr double kKappaDrift = 0.005; // mean-reversion of momentum (trends persist ~seconds)
constexpr double kSigmaDrift = 0.025; // momentum innovation (ticks/frame)
constexpr double kDriftClamp = 1.0;   // cap on drift (ticks/frame ~= $0.01 -> ~$0.6/s extreme)
constexpr double kFlowScale = 0.5;    // how sharply aggressive flow leans with drift
constexpr double kImpact = 0.015;     // permanent market impact per filled unit (ticks)
constexpr double kVolFloor = 0.25;    // ticks/frame
constexpr double kVolCeil = 20.0;
constexpr double kJumpPerFrame = 1.0 / 1500.0;  // spontaneous-news hazard per frame (~1 / 25s)

double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

// A limit order the *user* placed, tracked over its life so the UI can show how it
// is doing: how much has filled (immediately and later, as market flow reaches it),
// its average fill price, and whether it is done.
struct UserOrder {
    OrderId id;
    Side side;
    Price price;
    int orig;          // original quantity
    int filled = 0;    // cumulative filled quantity
    double notional = 0;  // sum of fill_price * fill_qty (tick units) -> average price
    bool done = false;
};
}  // namespace

class Sim {
public:
    // `start` is the tick the instrument opens at (e.g. tick for $100.00).
    Sim(int ticks, int start)
        : ticks_(ticks), book_(ticks, RESERVE), rng_(0x9E3779B9ull),
          start_(start), fv_(start) {
        logvol_mean_ = std::log(1.4);  // calm, until the UI sets turbulence
        logvol_ = logvol_mean_;
        vol_ = std::exp(logvol_);
        seed();
    }

    void reset() {
        book_ = OrderBook(ticks_, RESERVE);
        rng_.seed(0x9E3779B9ull);  // reproducible: reset yields the same market each time
        nd_.reset();
        live_.clear();
        mine_.clear();
        tape_.clear();
        next_id_ = 1;
        trades_ = 0;
        last_ = -1;
        fv_ = start_;
        drift_ = 0.0;
        logvol_ = logvol_mean_;
        vol_ = std::exp(logvol_);
        // the new book invalidates the maker's quote ids; clear its state (keep on/config)
        mm_bid_id_ = mm_ask_id_ = 0;
        mm_bid_px_ = mm_ask_px_ = -1;
        mmFlatten();
        seed();
    }

    // Manual order. type: 0 limit, 1 market, 2 ioc, 3 fok. side: 0 buy, 1 sell.
    val submit(int type, int side, int price, int qty) {
        // Enforce the order contract HERE, not in the UI: reject non-positive size,
        // cap the top, and keep price in the tick domain. A hostile/buggy caller must
        // not be able to wrap Qty (uint32) or overflow a level total.
        if (qty <= 0) {
            val r = val::object();
            r.set("filled", 0);
            r.set("resting", 0);
            r.set("rejected", true);
            return r;
        }
        if (qty > kMaxQty) qty = kMaxQty;
        if (price < 0) price = 0;
        if (price >= ticks_) price = ticks_ - 1;

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
                break;
        }
        record();
        double notional = 0;  // sum of immediate fill price*qty (ticks) -> average fill price
        for (const auto& f : fills_) notional += static_cast<double>(f.price) * f.qty;
        // Aggressive flow (what actually crossed) pushes fair value — market impact.
        const int filled = qty - static_cast<int>(rem);
        if (filled > 0) {
            fv_ += (s == Side::Buy ? 1.0 : -1.0) * kImpact * filled;
            clamp_fv();
        }
        // Track the user's limit orders so the UI can follow them over their life.
        // (Kept out of the sim's churn pool, so only genuine market flow fills the
        // resting remainder — not the simulator randomly cancelling it.)
        if (type == 0) {
            UserOrder u{id, s, static_cast<Price>(price), qty};
            u.filled = filled;      // immediate fills belong to this taker
            u.notional = notional;
            if (u.filled >= u.orig) u.done = true;
            mine_.push_front(u);
            while (mine_.size() > 6) {  // bound: expire the oldest working order
                const UserOrder old = mine_.back();
                mine_.pop_back();
                if (!old.done) book_.cancel(old.id);
            }
        }
        val r = val::object();
        r.set("id", static_cast<double>(id));
        r.set("filled", filled);
        r.set("resting", static_cast<int>(rem));
        r.set("avgTick", filled > 0 ? notional / filled : -1.0);
        return r;
    }

    // Cancel a resting order by id (used by the execution lab to pull a passive child).
    // Also marks any tracked working order done so the UI stops following it.
    bool cancelOrder(double idd) {
        const OrderId id = static_cast<OrderId>(idd);
        const bool ok = book_.cancel(id);
        for (auto& u : mine_)
            if (u.id == id) { u.done = true; break; }
        return ok;
    }

    // --- market maker: an automated two-sided quoter (see mm_* below) ---
    void mmEnable(bool on) {
        mm_on_ = on;
        if (!on) mm_cancel_quotes();  // stop quoting; keep inventory + realized PnL
    }
    void mmConfig(int half, int size, int invLimit) {
        mm_half_ = static_cast<Price>(half < 1 ? 1 : half);
        mm_size_ = size < 1 ? 1 : (size > kMaxQty ? kMaxQty : size);
        mm_invlimit_ = invLimit < 1 ? 1 : invLimit;
    }
    void mmFlatten() {  // reset the maker's book-keeping to flat (does not touch the market)
        mm_inv_ = 0;
        mm_avgcost_ = 0;
        mm_realized_ = 0;
        mm_spread_ = 0;
        mm_fills_ = 0;
        mm_volume_ = 0;
    }

    // One render frame: advance the price by dt (in 60fps-frame units of wall-clock
    // time), then run n order events around it. dt sets price velocity (refresh-rate
    // independent); n only sets activity.
    void step(int n, double dt) {
        evolve_price(dt);
        mm_requote();  // refresh the maker's two-sided quotes before this frame's flow
        for (int i = 0; i < n; ++i) one_step();
    }

    // A news shock: an instantaneous jump plus a decaying trend kick and a vol spike.
    // dir = +1 bullish, -1 bearish.
    void news(int dir) {
        const double d = dir >= 0 ? 1.0 : -1.0;
        const double jump = d * (30.0 + std::abs(nd_(rng_)) * 30.0);  // ~$0.30–$0.90
        fv_ += jump;
        drift_ += d * 0.8;                    // ignites a trend that mean-reverts away
        logvol_ = std::min(std::log(kVolCeil), logvol_ + 0.5);  // volatility jumps on news
        clamp_fv();
    }

    // Set the ambient turbulence, x in [0,1]: raises the mean of log-volatility.
    void setTurbulence(double x) {
        x = clampd(x, 0.0, 1.0);
        logvol_mean_ = std::log(0.6 + x * 7.4);  // calm ~0.6  ->  turbulent ~8 ticks/frame
    }

    // Full view for one render frame: L2 ladder + stats + tape + market state.
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

        double mid;
        if (book_.has_bid() && book_.has_ask())
            mid = (book_.best_bid() + book_.best_ask()) / 2.0;
        else if (last_ >= 0)
            mid = last_;
        else
            mid = fv_;
        o.set("midTick", mid);
        o.set("vol01", clampd((vol_ - 0.5) / 6.0, 0.0, 1.0));
        o.set("trend", clampd(drift_ / 0.8, -1.0, 1.0));

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

        val mine = val::array();
        for (const auto& u : mine_) {
            val e = val::object();
            e.set("id", static_cast<double>(u.id));
            e.set("side", u.side == Side::Buy ? 0 : 1);
            e.set("price", static_cast<int>(u.price));
            e.set("orig", u.orig);
            e.set("filled", u.filled);
            e.set("avgTick", u.filled > 0 ? u.notional / u.filled : -1.0);
            e.set("done", u.done);
            mine.call<void>("push", e);
        }
        o.set("mine", mine);

        // market-maker state (inventory, PnL decomposition, live quotes)
        val mm = val::object();
        mm.set("on", mm_on_);
        mm.set("inv", static_cast<double>(mm_inv_));
        mm.set("invLimit", static_cast<double>(mm_invlimit_));
        mm.set("half", static_cast<double>(mm_half_));
        mm.set("size", static_cast<double>(mm_size_));
        mm.set("bidPx", mm_bid_id_ ? static_cast<double>(mm_bid_px_) : -1.0);
        mm.set("askPx", mm_ask_id_ ? static_cast<double>(mm_ask_px_) : -1.0);
        // Total mark-to-market PnL = realized (closed lots) + unrealized (open MTM).
        // Decomposed honestly into spread capture (edge vs fair at each fill) and the
        // inventory / adverse-selection remainder (total − spread).
        const double unreal = static_cast<double>(mm_inv_) * (mid - mm_avgcost_);  // ticks*shares
        const double total = mm_realized_ + unreal;
        mm.set("pnlTick", total);
        mm.set("spreadTick", mm_spread_);
        mm.set("adverseTick", total - mm_spread_);
        mm.set("fills", static_cast<double>(mm_fills_));
        mm.set("volume", static_cast<double>(mm_volume_));
        o.set("mm", mm);
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
            // If a tracked user order rested here and market flow just hit it, book the fill.
            for (auto& u : mine_) {
                if (!u.done && u.id == f.maker) {
                    u.filled += static_cast<int>(f.qty);
                    u.notional += static_cast<double>(f.price) * f.qty;
                    if (u.filled >= u.orig) u.done = true;
                    break;
                }
            }
            // Book any fill involving the maker's orders into inventory/PnL. A fill's maker
            // and taker are distinct ids, so this chain books each fill exactly once — the
            // taker cases are a safety net in case a quote ever crosses (it shouldn't).
            if (f.maker == mm_bid_id_ || f.taker == mm_bid_id_)
                mm_apply_fill(static_cast<double>(f.price), static_cast<long long>(f.qty));  // maker bought
            else if (f.maker == mm_ask_id_ || f.taker == mm_ask_id_)
                mm_apply_fill(static_cast<double>(f.price), -static_cast<long long>(f.qty));  // maker sold
        }
        while (tape_.size() > 60) tape_.pop_back();
    }

    double u01() { return (rng_() % 1000000u) / 1000000.0; }

    void clamp_fv() {
        // A far safety bound at the array edges only — normal movement never reaches it.
        fv_ = clampd(fv_, 2.0, ticks_ - 3.0);
    }

    // Advance the latent market state by dt frame-equivalents: stochastic volatility
    // (clustering), an OU momentum/trend, rare fat-tailed news jumps, and diffusion.
    // Mean-reversion/drift scale with dt; diffusion noise with sqrt(dt) (Euler–Maruyama),
    // so the process is identical whether called at 60 or 240 Hz.
    void evolve_price(double dt) {
        if (dt <= 0.0) return;
        const double sdt = std::sqrt(dt);

        logvol_ += kKappaVol * (logvol_mean_ - logvol_) * dt + kSigmaVol * sdt * nd_(rng_);
        logvol_ = clampd(logvol_, std::log(kVolFloor), std::log(kVolCeil));
        vol_ = std::exp(logvol_);

        drift_ += -kKappaDrift * drift_ * dt + kSigmaDrift * sdt * nd_(rng_);
        drift_ = clampd(drift_, -kDriftClamp, kDriftClamp);

        if (u01() < kJumpPerFrame * dt) {  // spontaneous news (Poisson), ~ every 25s
            const double j = nd_(rng_) * 18.0;
            fv_ += j;
            drift_ += (j > 0 ? 1.0 : -1.0) * 0.8;
            logvol_ = std::min(std::log(kVolCeil), logvol_ + 0.5);
        }

        fv_ += drift_ * dt + vol_ * sdt * nd_(rng_);
        clamp_fv();
    }

    // Emit one order event around the current fair value (no price evolution here).
    void one_step() {
        fills_.clear();
        const double p_aggr_buy = 1.0 / (1.0 + std::exp(-drift_ / kFlowScale));  // informed flow leans with the trend
        const Price mid = static_cast<Price>(std::llround(fv_));
        const OrderId id = next_id_++;
        const int roll = static_cast<int>(rng_() % 100);

        // Realistic mix: mostly passive quoting and cancels, few marketable orders
        // (a high order-to-trade ratio, like a real book).
        if (roll < 55) {  // provide depth: a resting limit near the touch; spread widens with vol
            const bool buy = u01() < 0.5;
            const Price base = static_cast<Price>(1 + std::llround(vol_ / 4.0));  // ~1..6 ticks off
            const Price off = base + static_cast<Price>(rng_() % 3);
            const Price px = buy ? mid - off : mid + off;
            const Qty q = lot(35.0);
            if (book_.add_limit(id, buy ? Side::Buy : Side::Sell, px, q, fills_) > 0) live_.push_back(id);
        } else if (roll < 70) {  // take liquidity: a marketable order prints a trade
            const bool buy = u01() < p_aggr_buy;
            book_.add_market(id, buy ? Side::Buy : Side::Sell, lot(25.0), fills_);
        } else if (!live_.empty()) {  // churn: cancel a random resting order
            const std::size_t k = rng_() % live_.size();
            book_.cancel(live_[k]);
            live_[k] = live_.back();
            live_.pop_back();
        }
        if (live_.size() > 4000) {  // keep the book bounded (O(1) swap-pop, not erase(begin))
            book_.cancel(live_.front());
            live_.front() = live_.back();
            live_.pop_back();
        }
        record();
    }

    // A lognormal-ish lot size with the given median; clamped to a sane range.
    Qty lot(double median) {
        const double v = median * std::exp(nd_(rng_) * 0.7);
        return static_cast<Qty>(clampd(std::llround(v), 1.0, 400.0));
    }

    // ---- market maker ------------------------------------------------------
    void mm_cancel_quotes() {
        if (mm_bid_id_) { book_.cancel(mm_bid_id_); mm_bid_id_ = 0; mm_bid_px_ = -1; }
        if (mm_ask_id_) { book_.cancel(mm_ask_id_); mm_ask_id_ = 0; mm_ask_px_ = -1; }
    }

    // Refresh two-sided quotes each frame around a reservation price skewed by inventory
    // (Avellaneda–Stoikov style): the more inventory it holds, the more it shades quotes to
    // offload it. Hard inventory limit stops it quoting the side that would grow the position.
    void mm_requote() {
        mm_cancel_quotes();
        if (!mm_on_) return;
        // Clear the stale fill batch left by the previous frame's last one_step so the
        // record() below can't re-book it (double-counting trades/tape/user fills).
        fills_.clear();

        const double frac = clampd(static_cast<double>(mm_inv_) / static_cast<double>(mm_invlimit_), -1.0, 1.0);
        // inventory skew (reservation price, Avellaneda–Stoikov): shade quotes to offload.
        const double reservation = fv_ - frac * (2.0 * mm_half_ + 1.0);
        Price bidpx = static_cast<Price>(std::llround(reservation)) - mm_half_;
        Price askpx = static_cast<Price>(std::llround(reservation)) + mm_half_;
        // Stay strictly passive against the REAL book — never post through the touch and
        // accidentally take (the earlier fv-based clamp let stale liquidity be crossed).
        // Range-clamp FIRST, then re-assert the anti-cross so the boundary clamp can't
        // push a quote back onto the touch.
        bidpx = static_cast<Price>(clampd(bidpx, 0.0, ticks_ - 1.0));
        askpx = static_cast<Price>(clampd(askpx, 0.0, ticks_ - 1.0));
        if (book_.has_ask() && bidpx >= book_.best_ask()) bidpx = book_.best_ask() - 1;
        if (book_.has_bid() && askpx <= book_.best_bid()) askpx = book_.best_bid() + 1;

        // Hard inventory limit: cap each side's size to the remaining room, so a fill can
        // never push inventory past the bound (posting the full size could overshoot).
        const long long bidRoom = std::min<long long>(mm_size_, std::max<long long>(0, mm_invlimit_ - mm_inv_));
        const long long askRoom = std::min<long long>(mm_size_, std::max<long long>(0, mm_invlimit_ + mm_inv_));
        if (bidRoom > 0 && bidpx >= 0 && bidpx < ticks_) {
            mm_bid_id_ = next_id_++;
            book_.add_limit(mm_bid_id_, Side::Buy, bidpx, static_cast<Qty>(bidRoom), fills_);
            mm_bid_px_ = bidpx;
        }
        if (askRoom > 0 && askpx >= 0 && askpx < ticks_) {
            mm_ask_id_ = next_id_++;
            book_.add_limit(mm_ask_id_, Side::Sell, askpx, static_cast<Qty>(askRoom), fills_);
            mm_ask_px_ = askpx;
        }
        record();  // book any fill from this requote (posts are passive, but be safe)
    }

    // Book a maker fill into position PnL. q > 0 bought, q < 0 sold (both at px, in ticks).
    void mm_apply_fill(double px, long long q) {
        // Spread capture: the edge earned vs fair value at the instant of the fill (a bid
        // fill buys below fv, an ask fill sells above fv). The rest of the total PnL is the
        // inventory / adverse-selection component (total − spread), computed in snapshot().
        mm_spread_ += (fv_ - px) * static_cast<double>(q);
        if ((mm_inv_ > 0 && q < 0) || (mm_inv_ < 0 && q > 0)) {  // reducing/closing the position
            const long long closing = std::min(std::llabs(q), std::llabs(mm_inv_));
            const double sign = mm_inv_ > 0 ? 1.0 : -1.0;  // long: profit = px - avgcost
            mm_realized_ += sign * (px - mm_avgcost_) * static_cast<double>(closing);
        }
        const long long newInv = mm_inv_ + q;
        if (mm_inv_ == 0 || (mm_inv_ > 0 && q > 0) || (mm_inv_ < 0 && q < 0)) {  // adding to the position
            const double absOld = std::fabs(static_cast<double>(mm_inv_));
            const double absAdd = std::fabs(static_cast<double>(q));
            if (absOld + absAdd > 0) mm_avgcost_ = (mm_avgcost_ * absOld + px * absAdd) / (absOld + absAdd);
        } else if ((mm_inv_ > 0 && newInv < 0) || (mm_inv_ < 0 && newInv > 0)) {  // flipped through zero
            mm_avgcost_ = px;
        }
        mm_inv_ = newInv;
        if (mm_inv_ == 0) mm_avgcost_ = 0;
        ++mm_fills_;
        mm_volume_ += std::llabs(q);
    }

    void seed() {
        for (int i = 0; i < 400; ++i) one_step();
    }

    int ticks_;
    OrderBook book_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> nd_{0.0, 1.0};

    // latent market state
    double start_;              // opening fair value (ticks)
    double fv_;                 // fair value (fractional ticks)
    double drift_ = 0.0;        // momentum (ticks/frame)
    double vol_ = 4.0;          // instantaneous volatility (ticks/frame)
    double logvol_ = 0.0;       // log instantaneous volatility
    double logvol_mean_ = 0.0;  // target log-volatility (set by turbulence)

    std::vector<Trade> fills_;
    std::deque<Trade> tape_;
    std::deque<UserOrder> mine_;  // the user's tracked limit orders (newest first)
    std::vector<OrderId> live_;
    OrderId next_id_ = 1;
    std::uint64_t trades_ = 0;
    Price last_ = -1;

    // market maker
    bool mm_on_ = false;
    Price mm_half_ = 1;             // half-spread in ticks
    int mm_size_ = 200;            // quote size (shares per side)
    long long mm_invlimit_ = 2000;  // hard inventory bound (shares)
    OrderId mm_bid_id_ = 0, mm_ask_id_ = 0;  // 0 => not currently posted
    Price mm_bid_px_ = -1, mm_ask_px_ = -1;
    long long mm_inv_ = 0;          // signed inventory (shares)
    double mm_avgcost_ = 0;         // avg cost of the current inventory (ticks)
    double mm_realized_ = 0;        // realized PnL, closed lots (ticks*shares)
    double mm_spread_ = 0;          // cumulative spread capture vs fair (ticks*shares)
    long long mm_fills_ = 0;
    long long mm_volume_ = 0;
};

EMSCRIPTEN_BINDINGS(orderbook) {
    emscripten::class_<Sim>("Sim")
        .constructor<int, int>()
        .function("reset", &Sim::reset)
        .function("submit", &Sim::submit)
        .function("cancelOrder", &Sim::cancelOrder)
        .function("step", &Sim::step)  // step(n, dt)
        .function("news", &Sim::news)
        .function("setTurbulence", &Sim::setTurbulence)
        .function("mmEnable", &Sim::mmEnable)
        .function("mmConfig", &Sim::mmConfig)
        .function("mmFlatten", &Sim::mmFlatten)
        .function("snapshot", &Sim::snapshot);
}
