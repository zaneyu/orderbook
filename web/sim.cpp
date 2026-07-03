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

// --- price-process parameters (per micro-step; a frame runs ~250 of them) ---
constexpr double kKappaVol = 0.02;    // mean-reversion speed of log-volatility
constexpr double kSigmaVol = 0.08;    // vol-of-vol -> volatility *clusters*
constexpr double kKappaDrift = 0.003; // mean-reversion of momentum (small -> trends persist)
constexpr double kSigmaDrift = 0.004; // momentum innovation
constexpr double kDriftClamp = 0.09;  // cap on drift (ticks/step)
constexpr double kFlowScale = 0.03;   // how sharply aggressive flow leans with drift
constexpr double kImpact = 0.014;     // permanent market impact per filled unit (ticks)
constexpr double kVolFloor = 0.008;
constexpr double kVolCeil = 0.45;

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
    explicit Sim(int ticks)
        : ticks_(ticks), book_(ticks, RESERVE), rng_(0x9E3779B9ull), fv_(ticks / 2.0) {
        logvol_mean_ = std::log(0.055);
        logvol_ = logvol_mean_;
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
        fv_ = ticks_ / 2.0;
        drift_ = 0.0;
        logvol_ = logvol_mean_;
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

    void step(int n) {
        for (int i = 0; i < n; ++i) one_step();
    }

    // A news shock: an instantaneous jump plus a decaying trend kick and a vol spike.
    // dir = +1 bullish, -1 bearish.
    void news(int dir) {
        const double d = dir >= 0 ? 1.0 : -1.0;
        const double jump = d * (14.0 + std::abs(nd_(rng_)) * 12.0);  // ~0.14–0.40 in price
        fv_ += jump;
        drift_ += d * 0.045;                 // ignites a trend that mean-reverts away
        logvol_ = std::min(kVolCeil, logvol_ + 0.6);  // volatility jumps on news
        clamp_fv();
    }

    // Set the ambient turbulence, x in [0,1]: raises the mean of log-volatility.
    void setTurbulence(double x) {
        x = clampd(x, 0.0, 1.0);
        logvol_mean_ = std::log(0.02 + x * 0.26);  // calm ~0.02  ->  turbulent ~0.28
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
        o.set("vol01", clampd((std::exp(logvol_) - 0.02) / 0.20, 0.0, 1.0));
        o.set("trend", clampd(drift_ / 0.06, -1.0, 1.0));

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
        const double lo = 6.0, hi = ticks_ - 7.0;
        fv_ = clampd(fv_, lo, hi);
    }

    // Advance the latent market state by one micro-step, then emit one order.
    void one_step() {
        fills_.clear();

        // 1) stochastic volatility — log-vol mean-reverts with its own shocks (clustering)
        logvol_ += kKappaVol * (logvol_mean_ - logvol_) + kSigmaVol * nd_(rng_);
        logvol_ = clampd(logvol_, std::log(kVolFloor), std::log(kVolCeil));
        const double vol = std::exp(logvol_);

        // 2) momentum — an OU process; small mean-reversion means trends persist for a while
        drift_ += -kKappaDrift * drift_ + kSigmaDrift * nd_(rng_);
        drift_ = clampd(drift_, -kDriftClamp, kDriftClamp);

        // 3) rare fat-tailed jump (spontaneous news), with a matching vol spike
        if ((rng_() % 40000u) == 0u) {
            const double j = nd_(rng_) * 10.0;
            fv_ += j;
            drift_ += (j > 0 ? 1.0 : -1.0) * 0.02;
            logvol_ = std::min(std::log(kVolCeil), logvol_ + 0.5);
        }

        // 4) diffusion around the drifting fair value
        fv_ += drift_ + vol * nd_(rng_);
        clamp_fv();

        // --- order flow, conditioned on the state ---
        const double p_aggr_buy = 1.0 / (1.0 + std::exp(-drift_ / kFlowScale));  // informed flow leans with the trend
        const Price mid = static_cast<Price>(std::llround(fv_));
        const OrderId id = next_id_++;
        const int roll = static_cast<int>(rng_() % 100);

        if (roll < 46) {  // provide depth: a resting limit near the touch; spread widens with vol
            const bool buy = u01() < 0.5;
            const Price base = static_cast<Price>(1 + std::llround(vol / 0.02));  // 1..~13 ticks off
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
    double fv_;                 // fair value (fractional ticks)
    double drift_ = 0.0;        // momentum (ticks/step)
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
        .constructor<int>()
        .function("reset", &Sim::reset)
        .function("submit", &Sim::submit)
        .function("step", &Sim::step)
        .function("news", &Sim::news)
        .function("setTurbulence", &Sim::setTurbulence)
        .function("snapshot", &Sim::snapshot);
}
