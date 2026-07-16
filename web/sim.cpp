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
    bool protect = false;  // exempt from the tracked-order cap (execution-lab child)
};

// Signed-position PnL accounting (avg-cost basis, realized on closed lots). Shared by the
// autonomous strategies. All prices in ticks; realized in tick*shares. This is the same
// accounting a strict reviewer verified for the market maker (add/flip/close all correct).
struct Position {
    long long inv = 0;      // signed shares (+ long, - short)
    double avgcost = 0;     // avg cost of the current inventory (ticks)
    double realized = 0;    // realized PnL on closed lots (tick*shares)
    long long fills = 0;
    long long volume = 0;

    void apply(double px, long long q) {  // q > 0 bought, q < 0 sold
        if ((inv > 0 && q < 0) || (inv < 0 && q > 0)) {  // reducing/closing
            const long long closing = std::min(std::llabs(q), std::llabs(inv));
            const double sign = inv > 0 ? 1.0 : -1.0;    // long: profit = px - avgcost
            realized += sign * (px - avgcost) * static_cast<double>(closing);
        }
        const long long newInv = inv + q;
        if (inv == 0 || (inv > 0 && q > 0) || (inv < 0 && q < 0)) {  // adding to the position
            const double a = std::fabs(static_cast<double>(inv));
            const double b = std::fabs(static_cast<double>(q));
            if (a + b > 0) avgcost = (avgcost * a + px * b) / (a + b);
        } else if ((inv > 0 && newInv < 0) || (inv < 0 && newInv > 0)) {  // flipped through zero
            avgcost = px;
        }
        inv = newInv;
        if (inv == 0) avgcost = 0;
        ++fills;
        volume += std::llabs(q);
    }
    double mtm(double mid) const { return realized + static_cast<double>(inv) * (mid - avgcost); }
    void reset() { inv = 0; avgcost = 0; realized = 0; fills = 0; volume = 0; }
};

constexpr int kStratClip = 120;         // shares per strategy trade
constexpr long long kStratPos = 1500;   // strategy position limit (shares)
constexpr double kStratInterval = 5.0;  // frame-equivalents of wall-clock between trades
constexpr double kMomFast = 0.09;      // momentum fast-EMA alpha (per frame)
constexpr double kMomSlow = 0.012;     // momentum slow-EMA alpha
constexpr double kMomGain = 220.0;     // signal (ticks) -> target shares
constexpr double kMrAlpha = 0.02;      // mean-reversion mean-EMA alpha
constexpr double kMrGain = 260.0;      // deviation (ticks) -> target shares

// Avellaneda–Stoikov market-maker parameters. The maker sees ONLY observable state
// (book mid with its own quotes pulled, its own EWMA vol estimate) — never the sim's
// latent fair value. γ is risk aversion per clip of inventory, τ a rolling risk
// horizon in frame-equivalents; κ (fill-intensity decay) is implied by the UI
// half-spread knob via (1/γ)·ln(1+γ/κ) = half  ⇒  κ = γ/(e^{γ·half}−1) ≈ 1/half.
constexpr double kMmGamma = 0.005;     // risk aversion (ticks per clip per tick² of var·τ)
constexpr double kMmTau = 30.0;        // rolling horizon (frame-equivalents, ~0.5s)
constexpr double kMmVarAlpha = 0.05;   // EWMA alpha for the maker's own variance estimate
constexpr double kMmVarInit = 2.0;     // prior variance (ticks²/frame) before any samples
constexpr double kMmMaxSkew = 10.0;    // clamp on inventory skew (ticks) — demo tick budget
constexpr double kMmMaxWiden = 12.0;   // clamp on vol widening above the base half-spread
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
        mom_fast_ = mom_slow_ = mr_ema_ = anchor_ = start;
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
        evicted_ = 0;
        last_ = -1;
        fv_ = start_;
        drift_ = 0.0;
        logvol_ = logvol_mean_;
        vol_ = std::exp(logvol_);
        // the new book invalidates the maker's quote ids; clear its state (keep on/config)
        mm_bid_id_ = mm_ask_id_ = 0;
        mm_bid_px_ = mm_ask_px_ = -1;
        mm_var_ = kMmVarInit;
        mm_prev_mid_ = mm_ref_mid_ = -1.0;
        mm_acc_ = 0.0;
        ev_acc_ = 0.0;
        mmFlatten();
        stratFlatten();
        mom_fast_ = mom_slow_ = mr_ema_ = anchor_ = start_;
        mom_acc_ = mr_acc_ = 0;
        mr_k_ = 0.0;
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
            // Bound the tracked set at 6, but never silently kill live risk if a husk
            // will do: drop the oldest FILLED/CANCELLED entries first, and only then
            // cancel the oldest live order. Husk-dropping skips (a) protected entries —
            // the execution lab's child must survive until JS has read its final fills,
            // even if it filled and completed inside THIS submit call — and (b) the
            // just-pushed order at index 0, so an immediately-fully-filled limit still
            // renders once instead of vanishing before its first snapshot.
            for (std::size_t i = mine_.size(); i-- > 1 && mine_.size() > 6;)
                if (mine_[i].done && !mine_[i].protect) mine_.erase(mine_.begin() + i);
            while (mine_.size() > 6) {
                std::size_t v = mine_.size();
                for (std::size_t i = mine_.size(); i-- > 0;)
                    if (!mine_[i].protect) { v = i; break; }  // first hit = oldest unprotected
                if (v == mine_.size()) break;  // everything protected: let it grow instead
                book_.cancel(mine_[v].id);
                mine_.erase(mine_.begin() + v);
                ++evicted_;  // reported in snapshot(): the UI toasts on the counter, so a
                             // filled order purged as a husk can never false-flag as evicted
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

    // Exempt a tracked order from the 6-order cap (the execution lab's passive child:
    // evicting or husk-purging it mid-run would freeze/corrupt the lab's fill metrics).
    // The lab clears the flag when it finalizes, so done husks don't accumulate.
    void protectOrder(double idd, bool on) {
        const OrderId id = static_cast<OrderId>(idd);
        for (auto& u : mine_)
            if (u.id == id) { u.protect = on; break; }
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

    void momEnable(bool on) { mom_on_ = on; mom_acc_ = 0; }  // full interval before the first trade
    void mrEnable(bool on) { mr_on_ = on; mr_acc_ = 0; }
    void stratFlatten() {  // reset the momentum & mean-reversion book-keeping (PnL to flat)
        mom_pos_.reset();
        mr_pos_.reset();
    }

    // One render frame: advance the price by dt (in 60fps-frame units of wall-clock
    // time), then run order events around it. Activity is a RATE — n is events per
    // frame-equivalent, accumulated fractionally — so a 120 Hz display generates the
    // same flow per wall-second as a 60 Hz one, not double.
    void step(int n, double dt) {
        evolve_price(dt);
        run_strategies(dt); // momentum / mean-reversion take liquidity on their signal
        ev_acc_ += static_cast<double>(n) * dt;
        int m = static_cast<int>(ev_acc_);
        ev_acc_ -= m;
        if (m > n * 4 + 64) m = n * 4 + 64;  // dt is UI-capped at 4; belt and braces
        for (int i = 0; i < m; ++i) one_step();
        // The maker requotes LAST, off the observable post-flow book. Its quotes then
        // rest untouched through the whole of the next frame's price move, news, and
        // order flow — one frame of genuine latency — so, like any real quoter, it CAN
        // be picked off by a jump it hasn't reacted to yet.
        mm_requote(dt);
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

    // Preset market regimes so the strategies can be seen to win/lose by condition.
    void scenario(int id) {
        switch (id) {
            case 0:  // calm
                setTurbulence(0.08); drift_ = 0; mr_k_ = 0; break;
            case 1:  // trending (up)
                setTurbulence(0.20); drift_ = 0.7; mr_k_ = 0; break;
            case 2:  // choppy / mean-reverting
                setTurbulence(0.26); drift_ = 0; anchor_ = fv_; mr_k_ = 0.045; break;
            case 3:  // volatile
                setTurbulence(0.72); drift_ = 0; mr_k_ = 0; break;
            case 4:  // flash crash
                fv_ -= 110; drift_ = -1.0; mr_k_ = 0;
                logvol_ = std::log(kVolCeil); clamp_fv(); break;
            default: break;
        }
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
        o.set("evicted", static_cast<double>(evicted_));  // cumulative cap-evictions of LIVE orders

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
        mm.set("bidPx", mm_bid_id_ ? static_cast<double>(mm_bid_px_) : -1.0);
        mm.set("askPx", mm_ask_id_ ? static_cast<double>(mm_ask_px_) : -1.0);
        // Total mark-to-market PnL = realized (closed lots) + unrealized (open MTM).
        // Both buckets share ONE observable benchmark: the mid with the maker's own
        // quotes pulled (mm_ref_mid_). Marking at the raw book mid would let the maker's
        // own inventory skew move its mark; benchmarking spread at a different reference
        // than the mark would pollute the split between spread capture and adverse
        // selection, not just the labels.
        const double mark = mm_ref_mid_ >= 0.0 ? mm_ref_mid_ : mid;
        const double unreal = static_cast<double>(mm_inv_) * (mark - mm_avgcost_);  // ticks*shares
        const double total = mm_realized_ + unreal;
        mm.set("pnlTick", total);
        mm.set("spreadTick", mm_spread_);
        mm.set("adverseTick", total - mm_spread_);
        o.set("mm", mm);

        // autonomous strategy state (momentum, mean-reversion) for the strategies arena
        val strat = val::object();
        val mom = val::object();
        mom.set("on", mom_on_);
        mom.set("pnlTick", mom_pos_.mtm(mid));
        mom.set("inv", static_cast<double>(mom_pos_.inv));
        mom.set("signal", clampd((mom_fast_ - mom_slow_) / 6.0, -1.0, 1.0));
        strat.set("mom", mom);
        val mr = val::object();
        mr.set("on", mr_on_);
        mr.set("pnlTick", mr_pos_.mtm(mid));
        mr.set("inv", static_cast<double>(mr_pos_.inv));
        // report the bot's STANCE (negated deviation) so, like momentum, positive => wants long
        mr.set("signal", clampd(-(strat_mid() - mr_ema_) / 6.0, -1.0, 1.0));
        strat.set("mr", mr);
        o.set("strat", strat);
        return o;
    }

private:
    val side_levels(Side side, int depth) {
        val arr = val::array();
        book_.for_levels(side, depth, [&](Price p, std::uint64_t q) {  // u64 level totals
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

        // Drift + (optional) level mean-reversion toward an anchor + diffusion. mr_k_ is 0
        // by default (a trending random walk); the "choppy" scenario turns it on so the
        // price genuinely reverts — which is what gives a mean-reversion strategy an edge.
        fv_ += (drift_ - mr_k_ * (fv_ - anchor_)) * dt + vol_ * sdt * nd_(rng_);
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

    // Avellaneda–Stoikov quoting on OBSERVABLE state only — the maker never reads the
    // sim's latent fair value. On its (dt-accumulated, refresh-independent) cadence:
    //   reservation  r = m − q·γ·σ²·τ          (m = book mid with own quotes pulled,
    //                                            q = inventory in clips of quote size)
    //   half-spread  δ = γ·σ²·τ/2 + (1/γ)·ln(1 + γ/κ)
    // σ² is the maker's own EWMA estimate of per-frame mid variance, so spreads widen
    // in turbulence — the A-S signature. κ is implied by the UI half-spread knob
    // ((1/γ)·ln(1+γ/κ) = half), so δ = half + γ·σ²·τ/2. Skew and widening are clamped
    // to a demo-sane tick budget. Hard inventory limit still caps each side's size.
    void mm_requote(double dt) {
        mm_acc_ += dt;
        if (mm_acc_ < 1.0) return;  // one frame-equivalent between requotes, at any Hz
        const double elapsed = mm_acc_;
        mm_acc_ = 0.0;

        mm_cancel_quotes();
        // Observable mid with our own quotes pulled: quoting or benchmarking against a
        // mid made of our own orders would be self-referential.
        const double m = obs_mid();
        if (mm_prev_mid_ >= 0.0 && elapsed > 0.0) {
            const double d = m - mm_prev_mid_;
            const double inst = d * d / elapsed;                    // per-frame variance sample
            mm_var_ += -std::expm1(-kMmVarAlpha * elapsed) * (inst - mm_var_);
        }
        mm_prev_mid_ = m;
        mm_ref_mid_ = m;  // benchmark for fills against the quotes posted below
        if (!mm_on_) return;
        // Clear the stale fill batch left by the previous one_step so the record()
        // below can't re-book it (double-counting trades/tape/user fills).
        fills_.clear();

        const double qClips = static_cast<double>(mm_inv_) / static_cast<double>(mm_size_);
        const double skew = clampd(kMmGamma * mm_var_ * kMmTau * qClips, -kMmMaxSkew, kMmMaxSkew);
        const double widen = clampd(kMmGamma * mm_var_ * kMmTau * 0.5, 0.0, kMmMaxWiden);
        const double reservation = m - skew;
        const double half = static_cast<double>(mm_half_) + widen;
        Price bidpx = static_cast<Price>(std::llround(reservation - half));
        Price askpx = static_cast<Price>(std::llround(reservation + half));
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
        // Spread capture: the edge vs the OBSERVABLE mid the maker quoted around (a bid
        // fill buys below it, an ask fill sells above it) — not the sim's latent fair
        // value, which would launder adverse selection out of the split. The rest of the
        // total PnL is the inventory / adverse-selection component, computed in snapshot().
        const double bench = mm_ref_mid_ >= 0.0 ? mm_ref_mid_ : px;
        mm_spread_ += (bench - px) * static_cast<double>(q);
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

    // The mid every bot is allowed to see: book mid, else the last trade, else the
    // open. NEVER the latent fv_ — no strategy in the sim trades on an oracle.
    double obs_mid() const {
        if (book_.has_bid() && book_.has_ask())
            return (book_.best_bid() + book_.best_ask()) / 2.0;
        if (last_ >= 0) return static_cast<double>(last_);
        return start_;
    }

    // ---- autonomous taker strategies (momentum, mean-reversion) ------------
    double strat_mid() const { return obs_mid(); }

    // Move `pos` toward a target position by taking a clip of liquidity (a market order).
    // A deadband avoids churn. Returns true if it traded.
    bool strat_trade(Position& pos, double target) {
        const long long diff = static_cast<long long>(std::llround(target)) - pos.inv;
        if (std::llabs(diff) < kStratClip / 2) return false;
        const long long q = std::max<long long>(-kStratClip, std::min<long long>(kStratClip, diff));
        if (q == 0) return false;
        const bool buy = q > 0;
        fills_.clear();
        const OrderId id = next_id_++;
        book_.add_market(id, buy ? Side::Buy : Side::Sell, static_cast<Qty>(std::llabs(q)), fills_);
        long long got = 0;
        for (const auto& f : fills_) {
            pos.apply(static_cast<double>(f.price), buy ? static_cast<long long>(f.qty) : -static_cast<long long>(f.qty));
            got += f.qty;
        }
        record();  // update tape/trades + attribute the maker/user side, at the pre-impact fair
        if (got > 0) {  // then move fair value — strategy flow exerts market impact like any aggressor
            fv_ += (buy ? 1.0 : -1.0) * kImpact * got;
            clamp_fv();
        }
        return true;
    }

    void run_strategies(double dt) {
        // Advance the EMAs every frame (even when the bots are off) so they never freeze
        // stale and snap on re-enable. The decay is 1−e^{−α·dt} (exact, not the first-
        // order α·dt), so the effective smoothing is identical at 60 or 240 Hz.
        const double mid = strat_mid();
        mom_fast_ += (mid - mom_fast_) * -std::expm1(-kMomFast * dt);
        mom_slow_ += (mid - mom_slow_) * -std::expm1(-kMomSlow * dt);
        mr_ema_ += (mid - mr_ema_) * -std::expm1(-kMrAlpha * dt);

        // Trade on a WALL-CLOCK cadence (accumulated dt), not per render frame, so turnover
        // and impact are identical at 60 or 240 Hz — the same invariant as the price process.
        if (mom_on_) {
            mom_acc_ += dt;
            if (mom_acc_ >= kStratInterval) {
                mom_acc_ = 0;
                const double signal = mom_fast_ - mom_slow_;  // buy strength / sell weakness
                const double target = clampd(signal * kMomGain, -static_cast<double>(kStratPos), static_cast<double>(kStratPos));
                strat_trade(mom_pos_, target);
            }
        }
        if (mr_on_) {
            mr_acc_ += dt;
            if (mr_acc_ >= kStratInterval) {
                mr_acc_ = 0;
                const double dev = mid - mr_ema_;  // fade deviations from the running mean
                const double target = clampd(-dev * kMrGain, -static_cast<double>(kStratPos), static_cast<double>(kStratPos));
                strat_trade(mr_pos_, target);
            }
        }
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
    std::uint64_t evicted_ = 0;  // live orders cancelled by the tracked-order cap
    Price last_ = -1;

    // market maker
    bool mm_on_ = false;
    Price mm_half_ = 1;             // base (minimum) half-spread in ticks — implies κ
    int mm_size_ = 200;            // quote size (shares per side; one "clip")
    long long mm_invlimit_ = 2000;  // hard inventory bound (shares)
    OrderId mm_bid_id_ = 0, mm_ask_id_ = 0;  // 0 => not currently posted
    Price mm_bid_px_ = -1, mm_ask_px_ = -1;
    long long mm_inv_ = 0;          // signed inventory (shares)
    double mm_avgcost_ = 0;         // avg cost of the current inventory (ticks)
    double mm_realized_ = 0;        // realized PnL, closed lots (ticks*shares)
    double mm_spread_ = 0;          // cumulative spread capture vs quoted mid (ticks*shares)
    long long mm_fills_ = 0;
    long long mm_volume_ = 0;
    double mm_var_ = kMmVarInit;    // maker's own EWMA per-frame mid-variance estimate
    double mm_prev_mid_ = -1.0;     // last observed mid (for the variance estimator)
    double mm_ref_mid_ = -1.0;      // observable mid at the last requote: fill benchmark + mark
    double mm_acc_ = 0.0;           // dt accumulator: requote cadence, refresh-independent
    double ev_acc_ = 0.0;           // dt accumulator: fractional order-event budget

    // autonomous taker strategies
    bool mom_on_ = false;
    Position mom_pos_;
    double mom_acc_ = 0;                   // wall-clock (frame-equiv) since last trade
    double mom_fast_ = 0, mom_slow_ = 0;   // fast/slow EMAs of the mid (ticks)
    bool mr_on_ = false;
    Position mr_pos_;
    double mr_acc_ = 0;
    double mr_ema_ = 0;                    // running-mean EMA of the mid (ticks)

    // scenario-driven level mean-reversion of the price
    double mr_k_ = 0.0;   // reversion strength (0 => trending random walk)
    double anchor_ = 0.0; // level the price reverts toward when mr_k_ > 0
};

EMSCRIPTEN_BINDINGS(orderbook) {
    emscripten::class_<Sim>("Sim")
        .constructor<int, int>()
        .function("reset", &Sim::reset)
        .function("submit", &Sim::submit)
        .function("cancelOrder", &Sim::cancelOrder)
        .function("protectOrder", &Sim::protectOrder)
        .function("step", &Sim::step)  // step(n, dt)
        .function("news", &Sim::news)
        .function("setTurbulence", &Sim::setTurbulence)
        .function("mmEnable", &Sim::mmEnable)
        .function("mmConfig", &Sim::mmConfig)
        .function("mmFlatten", &Sim::mmFlatten)
        .function("momEnable", &Sim::momEnable)
        .function("mrEnable", &Sim::mrEnable)
        .function("stratFlatten", &Sim::stratFlatten)
        .function("scenario", &Sim::scenario)
        .function("snapshot", &Sim::snapshot);
}
