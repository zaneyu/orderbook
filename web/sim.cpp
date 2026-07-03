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

// The price process is advanced by WALL-CLOCK time (a `dt` in 60fps-frame units), not
// per render frame or per order event — so price velocity is identical at any refresh
// rate, and the "speed" control changes market activity, not how fast the price moves.
// Units are ticks (1 tick = $0.01), rates per frame-equivalent. Calibrated for a ~$100
// instrument: lively but realistic (tens of cents/sec, trends lasting several seconds).
constexpr double kKappaVol = 0.02;    // mean-reversion speed of log-volatility
constexpr double kSigmaVol = 0.13;    // vol-of-vol -> volatility *clusters*
constexpr double kKappaDrift = 0.006; // mean-reversion of momentum (trends persist ~seconds)
constexpr double kSigmaDrift = 0.07;  // momentum innovation (ticks/frame)
constexpr double kDriftClamp = 2.5;   // cap on drift (ticks/frame ~= $0.025 -> ~$1.5/s)
constexpr double kFlowScale = 0.9;    // how sharply aggressive flow leans with drift
constexpr double kImpact = 0.05;      // permanent market impact per filled unit (ticks)
constexpr double kVolFloor = 0.3;     // ticks/frame
constexpr double kVolCeil = 25.0;
constexpr double kJumpPerFrame = 1.0 / 600.0;  // spontaneous-news hazard per frame (~1 / 10s)

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
        logvol_mean_ = std::log(2.5);  // until the UI sets turbulence
        logvol_ = logvol_mean_;
        vol_ = std::exp(logvol_);
        seed();
    }

    void reset() {
        book_ = OrderBook(ticks_, RESERVE);
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
                break;
        }
        record();
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
            for (const auto& f : fills_) {  // immediate fills belong to this taker
                u.filled += static_cast<int>(f.qty);
                u.notional += static_cast<double>(f.price) * f.qty;
            }
            if (u.filled >= u.orig) u.done = true;
            mine_.push_front(u);
            while (mine_.size() > 6) {  // bound: expire the oldest working order
                const UserOrder old = mine_.back();
                mine_.pop_back();
                if (!old.done) book_.cancel(old.id);
            }
        }
        val r = val::object();
        r.set("filled", filled);
        r.set("resting", static_cast<int>(rem));
        return r;
    }

    // One render frame: advance the price by dt (in 60fps-frame units of wall-clock
    // time), then run n order events around it. dt sets price velocity (refresh-rate
    // independent); n only sets activity.
    void step(int n, double dt) {
        evolve_price(dt);
        for (int i = 0; i < n; ++i) one_step();
    }

    // A news shock: an instantaneous jump plus a decaying trend kick and a vol spike.
    // dir = +1 bullish, -1 bearish.
    void news(int dir) {
        const double d = dir >= 0 ? 1.0 : -1.0;
        const double jump = d * (40.0 + std::abs(nd_(rng_)) * 40.0);  // ~$0.40–$1.20
        fv_ += jump;
        drift_ += d * 1.5;                    // ignites a trend that mean-reverts away
        logvol_ = std::min(std::log(kVolCeil), logvol_ + 0.6);  // volatility jumps on news
        clamp_fv();
    }

    // Set the ambient turbulence, x in [0,1]: raises the mean of log-volatility.
    void setTurbulence(double x) {
        x = clampd(x, 0.0, 1.0);
        logvol_mean_ = std::log(1.0 + x * 9.0);  // calm ~1  ->  turbulent ~10 ticks/frame
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
        o.set("vol01", clampd((vol_ - 1.0) / 9.0, 0.0, 1.0));
        o.set("trend", clampd(drift_ / 2.0, -1.0, 1.0));

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
            e.set("side", u.side == Side::Buy ? 0 : 1);
            e.set("price", static_cast<int>(u.price));
            e.set("orig", u.orig);
            e.set("filled", u.filled);
            e.set("avgTick", u.filled > 0 ? u.notional / u.filled : -1.0);
            e.set("done", u.done);
            mine.call<void>("push", e);
        }
        o.set("mine", mine);
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

        if (u01() < kJumpPerFrame * dt) {  // spontaneous news (Poisson), ~ every 10s
            const double j = nd_(rng_) * 25.0;
            fv_ += j;
            drift_ += (j > 0 ? 1.0 : -1.0) * 1.5;
            logvol_ = std::min(std::log(kVolCeil), logvol_ + 0.6);
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

        if (roll < 46) {  // provide depth: a resting limit near the touch; spread widens with vol
            const bool buy = u01() < 0.5;
            const Price base = static_cast<Price>(1 + std::llround(vol_ / 3.0));  // 1..~7 ticks off
            const Price off = base + static_cast<Price>(rng_() % 3);
            const Price px = buy ? mid - off : mid + off;
            const Qty q = lot(10.0);
            if (book_.add_limit(id, buy ? Side::Buy : Side::Sell, px, q, fills_) > 0) live_.push_back(id);
        } else if (roll < 72) {  // take liquidity: a marketable order prints a trade
            const bool buy = u01() < p_aggr_buy;
            book_.add_market(id, buy ? Side::Buy : Side::Sell, lot(6.0), fills_);
        } else if (!live_.empty()) {  // churn: cancel a random resting order
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

    // A lognormal-ish lot size with the given median; clamped to a sane range.
    Qty lot(double median) {
        const double v = median * std::exp(nd_(rng_) * 0.7);
        return static_cast<Qty>(clampd(std::llround(v), 1.0, 60.0));
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
};

EMSCRIPTEN_BINDINGS(orderbook) {
    emscripten::class_<Sim>("Sim")
        .constructor<int, int>()
        .function("reset", &Sim::reset)
        .function("submit", &Sim::submit)
        .function("step", &Sim::step)  // step(n, dt)
        .function("news", &Sim::news)
        .function("setTurbulence", &Sim::setTurbulence)
        .function("snapshot", &Sim::snapshot);
}
