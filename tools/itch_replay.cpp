// Replay a NASDAQ TotalView-ITCH 5.0 day through the matching engine and report
// throughput plus the validation counters (see itch/book_builder.hpp for what each
// one proves). Reads the raw length-prefixed file from a path or stdin, so the
// multi-GB .gz never needs to land on disk uncompressed:
//
//   gunzip -c 12302019.NASDAQ_ITCH50.gz | ./itch_replay --symbols AAPL,MSFT,SPY -
//   ./itch_replay day.itch                 (uncompressed file)
//
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "itch/book_builder.hpp"
#include "itch/itch.hpp"

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
                 "usage: itch_replay [--symbols A,B,C] <file | ->\n"
                 "  default symbols: %s\n",
                 kDefaultSymbols);
}

double pct(std::uint64_t num, std::uint64_t den) {
    return den == 0 ? 0.0 : 100.0 * static_cast<double>(num) / static_cast<double>(den);
}

}  // namespace

int main(int argc, char** argv) {
    const char* symbols = kDefaultSymbols;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
            symbols = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            usage();
            return 2;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
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
    itch::Reader reader(f);

    const auto t0 = std::chrono::steady_clock::now();
    itch::MsgView m;
    while (reader.next(m)) builder.apply(m);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    const auto& s = builder.stats();
    std::printf("itch_replay: %llu messages in %.2fs  ->  %.1f M msgs/s%s\n",
                static_cast<unsigned long long>(s.msgs), secs, s.msgs / secs / 1e6,
                reader.truncated() ? "  (stream truncated mid-record)" : "");
    std::printf("\napplied to %zu tracked books:\n", builder.num_books());
    std::printf("  adds %llu   execs %llu   partial-cancels %llu   deletes %llu   replaces %llu   hidden-trades %llu\n",
                static_cast<unsigned long long>(s.adds), static_cast<unsigned long long>(s.execs),
                static_cast<unsigned long long>(s.cancels), static_cast<unsigned long long>(s.deletes),
                static_cast<unsigned long long>(s.replaces),
                static_cast<unsigned long long>(s.hidden_trades));

    std::printf("\nvalidation (all should be ~0):\n");
    std::printf("  unknown refs %llu   rest rejected %llu   qty underflow %llu   crossing adds (RTH) %llu   malformed %llu\n",
                static_cast<unsigned long long>(s.unknown_ref),
                static_cast<unsigned long long>(s.rest_rejected),
                static_cast<unsigned long long>(s.qty_underflow),
                static_cast<unsigned long long>(s.crossing_adds_rth),
                static_cast<unsigned long long>(s.malformed));
    std::printf("  skipped: sub-penny %llu   out-of-ladder %llu   (their later events: %llu, excluded above)\n",
                static_cast<unsigned long long>(s.subpenny_skipped),
                static_cast<unsigned long long>(s.out_of_band),
                static_cast<unsigned long long>(s.ignored_events));

    std::printf("\nfront-of-queue fidelity (regular hours, uncrossed book):\n");
    std::printf("  clean levels (steady state, no released orders): %llu executions,"
                " %.4f%% hit the head of our best-price FIFO exactly\n",
                static_cast<unsigned long long>(s.e_clean),
                pct(s.e_clean_front_best, s.e_clean));
    std::printf("  all steady state (>5min after open):             %llu executions, %.4f%%\n",
                static_cast<unsigned long long>(s.e_steady),
                pct(s.e_steady_front_best, s.e_steady));
    std::printf("  incl. the open window:                           %llu executions, %.4f%%\n",
                static_cast<unsigned long long>(s.e_rth), pct(s.e_front_best, s.e_rth));
    std::printf("  released/re-displayed adds detected: %llu (their exchange priority is"
                " not public; levels holding one are excluded from 'clean')\n",
                static_cast<unsigned long long>(s.released_adds));
    std::printf("  misses: %llu head-of-level-not-best, %llu second-in-queue, %llu deeper"
                "   (crossed-book execs excluded: %llu)\n",
                static_cast<unsigned long long>(s.e_front_only),
                static_cast<unsigned long long>(s.e_miss_second),
                static_cast<unsigned long long>(s.e_miss_deeper),
                static_cast<unsigned long long>(s.e_crossed));

    std::printf("\nend of stream: %zu orders still resting across books\n", builder.tracked_orders());
    for (std::size_t i = 0; i < builder.num_books(); ++i) {
        const auto& b = builder.book(i);
        std::printf("  %-8s resting %-7zu", builder.name(i).c_str(), b.resting_orders());
        if (b.has_bid() && b.has_ask())
            std::printf(" last book %.2f / %.2f", b.best_bid() / 100.0, b.best_ask() / 100.0);
        std::printf("\n");
    }

    if (f != stdin) std::fclose(f);
    return 0;
}
