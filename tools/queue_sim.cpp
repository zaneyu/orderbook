// Queue-position / fill-probability study over a real ITCH day.
//
// Replays the feed through the book builder with a QueueSim sink: phantom passive
// orders join the back of the best-bid queue at a fixed market-time cadence and are
// tracked EXACTLY against the L3 flow (see itch/queue_sim.hpp for the model and its
// one-sided assumption). Emits one CSV row per trial and prints an aggregate table:
// fill probability and time-to-first-fill by queue-depth-at-insertion decile.
//
//   gunzip -c day.gz | ./queue_sim --symbols AAPL,MSFT --csv trials.csv -
//
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "itch/book_builder.hpp"
#include "itch/itch.hpp"
#include "itch/queue_sim.hpp"

namespace {

std::vector<std::string> split_symbols(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

const char* kDefaultSymbols = "AAPL,MSFT,INTC,AMD,NVDA,TSLA,CSCO,QQQ,SPY,FB";

void usage() {
    std::fprintf(stderr,
                 "usage: queue_sim [--symbols A,B,C] [--csv out.csv] [--qty N]\n"
                 "                 [--every SECONDS] [--horizon SECONDS] <file | ->\n");
}

const char* outcome_of(const itch::QueueSim::Trial& t, std::uint32_t qty) {
    if (t.traded_through) return "traded_through";
    if (t.remaining == 0) return "filled";
    if (t.remaining < qty) return "partial";
    return "expired";
}

}  // namespace

int main(int argc, char** argv) {
    const char* symbols = kDefaultSymbols;
    const char* path = nullptr;
    const char* csv_path = nullptr;
    itch::QueueSim::Config qcfg;
    for (int i = 1; i < argc; ++i) {
        auto arg_val = [&](const char* flag) -> const char* {
            if (std::strcmp(argv[i], flag) == 0 && i + 1 < argc) return argv[++i];
            return nullptr;
        };
        if (const char* v = arg_val("--symbols")) symbols = v;
        else if (const char* v = arg_val("--csv")) csv_path = v;
        else if (const char* v = arg_val("--qty")) qcfg.phantom_qty = static_cast<std::uint32_t>(std::atoi(v));
        else if (const char* v = arg_val("--every")) qcfg.sample_every_ns = static_cast<std::uint64_t>(std::atof(v) * 1e9);
        else if (const char* v = arg_val("--horizon")) qcfg.horizon_ns = static_cast<std::uint64_t>(std::atof(v) * 1e9);
        else if (argv[i][0] == '-' && argv[i][1] != '\0') { usage(); return 2; }
        else path = argv[i];
    }
    if (!path || qcfg.phantom_qty == 0) {
        usage();
        return 2;
    }

    std::FILE* f = (std::strcmp(path, "-") == 0) ? stdin : std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }

    itch::BookBuilder::Config cfg;
    cfg.symbols = split_symbols(symbols);
    itch::BookBuilder builder(std::move(cfg));
    itch::QueueSim sim(qcfg, &builder);  // sim snapshots queues from the builder...
    builder.set_sink(&sim);              // ...and the builder feeds events to the sim

    itch::Reader reader(f);
    const auto t0 = std::chrono::steady_clock::now();
    itch::MsgView m;
    while (reader.next(m)) builder.apply(m);
    sim.flush(builder.last_ts());
    const auto t1 = std::chrono::steady_clock::now();

    // ---- per-trial CSV -----------------------------------------------------------
    if (csv_path) {
        std::FILE* out = std::fopen(csv_path, "w");
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", csv_path);
            return 1;
        }
        std::fprintf(out,
                     "symbol,t_insert_ns,price_ticks,ahead_shares,ahead_orders,spread_ticks,"
                     "filled_shares,t_first_fill_ms,t_close_ms,outcome,anomalies\n");
        for (const auto& t : sim.trials()) {
            std::fprintf(out, "%s,%llu,%d,%llu,%u,%d,%u,%.3f,%.3f,%s,%u\n",
                         builder.name(t.book).c_str(),
                         static_cast<unsigned long long>(t.t0), t.price,
                         static_cast<unsigned long long>(t.ahead0), t.norders0, t.spread0,
                         qcfg.phantom_qty - t.remaining,
                         t.t_first ? (t.t_first - t.t0) / 1e6 : -1.0,
                         t.t_done ? (t.t_done - t.t0) / 1e6 : -1.0,
                         outcome_of(t, qcfg.phantom_qty), t.anomalies);
        }
        std::fclose(out);
    }

    // ---- aggregate: fill probability by queue-depth decile -------------------------
    std::vector<const itch::QueueSim::Trial*> ts;
    std::uint64_t anomalies = 0;
    for (const auto& t : sim.trials()) {
        ts.push_back(&t);
        anomalies += t.anomalies;
    }
    std::sort(ts.begin(), ts.end(),
              [](const auto* a, const auto* b) { return a->ahead0 < b->ahead0; });

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("queue_sim: %llu messages, %zu trials (%u sh phantoms, every %.0fs, %.0fs horizon) in %.1fs\n",
                static_cast<unsigned long long>(builder.stats().msgs), ts.size(),
                qcfg.phantom_qty, qcfg.sample_every_ns / 1e9, qcfg.horizon_ns / 1e9, secs);
    std::printf("event-order anomalies: %llu (execs at our price behind us while queue ahead — should be ~0)\n\n",
                static_cast<unsigned long long>(anomalies));
    if (ts.empty()) return 0;

    std::printf("fill probability by queue-ahead-at-insertion decile (join the best bid):\n");
    std::printf("  %-8s %12s %10s %10s %10s %14s\n", "decile", "ahead (sh)", "P(fill>0)",
                "P(full)", "P(thru)", "median t1 (s)");
    const std::size_t n = ts.size();
    for (int d = 0; d < 10; ++d) {
        const std::size_t lo = n * d / 10, hi = n * (d + 1) / 10;
        if (lo >= hi) continue;
        std::size_t any = 0, full = 0, thru = 0;
        std::vector<double> t1s;
        for (std::size_t i = lo; i < hi; ++i) {
            const auto& t = *ts[i];
            const bool filled_any = t.remaining < qcfg.phantom_qty;
            any += filled_any;
            full += (t.remaining == 0);
            thru += t.traded_through;
            if (t.t_first) t1s.push_back((t.t_first - t.t0) / 1e9);
        }
        double med = -1;
        if (!t1s.empty()) {
            std::nth_element(t1s.begin(), t1s.begin() + t1s.size() / 2, t1s.end());
            med = t1s[t1s.size() / 2];
        }
        const std::size_t cnt = hi - lo;
        std::printf("  %-8d %5llu-%-6llu %9.1f%% %9.1f%% %9.1f%% %14.2f\n", d + 1,
                    static_cast<unsigned long long>(ts[lo]->ahead0),
                    static_cast<unsigned long long>(ts[hi - 1]->ahead0),
                    100.0 * any / cnt, 100.0 * full / cnt, 100.0 * thru / cnt, med);
    }

    if (f != stdin) std::fclose(f);
    return 0;
}
