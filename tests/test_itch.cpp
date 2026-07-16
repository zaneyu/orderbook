// Tests for the ITCH 5.0 stack: parser (synthetic binary fixtures through the real
// Reader), book builder (state + validation counters), and the queue-position
// simulator (hand-computed scenarios for every fill/expiry rule).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "framework.hpp"
#include "itch/book_builder.hpp"
#include "itch/itch.hpp"
#include "itch/queue_sim.hpp"

using namespace itch;

namespace {

// ---- binary fixture helpers --------------------------------------------------------
struct Wire {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
    void u32(std::uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void u48(std::uint64_t v) { u16((v >> 32) & 0xFFFF); u32(v & 0xFFFFFFFF); }
    void u64(std::uint64_t v) { u32(v >> 32); u32(v & 0xFFFFFFFF); }
    void str8(const char* s) {  // 8 chars, space-padded
        std::size_t i = 0;
        for (; s[i] && i < 8; ++i) u8(static_cast<std::uint8_t>(s[i]));
        for (; i < 8; ++i) u8(' ');
    }
};

struct Stream {
    std::vector<std::uint8_t> bytes;
    void put(const Wire& m) {
        bytes.push_back(static_cast<std::uint8_t>(m.bytes.size() >> 8));
        bytes.push_back(static_cast<std::uint8_t>(m.bytes.size() & 0xFF));
        bytes.insert(bytes.end(), m.bytes.begin(), m.bytes.end());
    }

    void sys(std::uint64_t ts, char code) {
        Wire m; m.u8('S'); m.u16(0); m.u16(0); m.u48(ts); m.u8(code); put(m);
    }
    void dir(std::uint16_t locate, const char* stock) {
        Wire m; m.u8('R'); m.u16(locate); m.u16(0); m.u48(0); m.str8(stock);
        while (m.bytes.size() < 39) m.u8(0);  // fields we don't decode
        put(m);
    }
    void add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, char side,
             std::uint32_t shares, const char* stock, std::uint32_t price4) {
        Wire m; m.u8('A'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(ref);
        m.u8(static_cast<std::uint8_t>(side)); m.u32(shares); m.str8(stock); m.u32(price4);
        put(m);
    }
    void exec(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        Wire m; m.u8('E'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(ref);
        m.u32(shares); m.u64(777);  // match number
        put(m);
    }
    void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        Wire m; m.u8('X'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(ref); m.u32(shares);
        put(m);
    }
    void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        Wire m; m.u8('D'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(ref);
        put(m);
    }
    void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t orig,
                 std::uint64_t nref, std::uint32_t shares, std::uint32_t price4) {
        Wire m; m.u8('U'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(orig); m.u64(nref);
        m.u32(shares); m.u32(price4);
        put(m);
    }
};

// Feed a byte stream through the real Reader into the builder.
void run(const Stream& s, BookBuilder& b) {
    std::FILE* f = fmemopen(const_cast<std::uint8_t*>(s.bytes.data()), s.bytes.size(), "rb");
    Reader r(f);
    MsgView m;
    while (r.next(m)) b.apply(m);
    std::fclose(f);
}

constexpr std::uint64_t SEC = 1'000'000'000ull;

BookBuilder::Config one_symbol() {
    BookBuilder::Config c;
    c.symbols = {"AAPL"};
    c.num_ticks = 1 << 18;
    c.reserve_orders = 1 << 12;
    return c;
}

}  // namespace

// -------------------------------------------------------------- parser round-trips

TEST(itch_reader_parses_length_prefixed_stream) {
    Stream s;
    s.sys(1 * SEC, 'Q');
    s.add(7, 2 * SEC, 42, 'B', 100, "AAPL", 1000000);  // $100.0000
    std::FILE* f = fmemopen(s.bytes.data(), s.bytes.size(), "rb");
    Reader r(f);
    MsgView m;
    CHECK(r.next(m));
    CHECK_EQ(m.type(), 'S');
    const auto sys = decode_system(m.p);
    CHECK_EQ(sys.ts, 1 * SEC);
    CHECK_EQ(sys.code, 'Q');
    CHECK(r.next(m));
    CHECK_EQ(m.type(), 'A');
    const auto a = decode_add(m.p);
    CHECK_EQ(a.locate, std::uint16_t{7});
    CHECK_EQ(a.ts, 2 * SEC);
    CHECK_EQ(a.ref, std::uint64_t{42});
    CHECK_EQ(a.side, 'B');
    CHECK_EQ(a.shares, std::uint32_t{100});
    CHECK_EQ(a.price4, std::uint32_t{1000000});
    CHECK(!r.next(m));       // clean EOF
    CHECK(!r.truncated());
    CHECK_EQ(r.count(), std::uint64_t{2});
    std::fclose(f);
}

TEST(itch_reader_flags_truncated_stream) {
    Stream s;
    s.add(7, 1, 42, 'B', 100, "AAPL", 1000000);
    s.add(7, 2, 43, 'S', 100, "AAPL", 1000100);
    std::vector<std::uint8_t> cut(s.bytes.begin(), s.bytes.end() - 5);  // mid-record
    std::FILE* f = fmemopen(cut.data(), cut.size(), "rb");
    Reader r(f);
    MsgView m;
    CHECK(r.next(m));   // first message intact
    CHECK(!r.next(m));  // second is cut
    CHECK(r.truncated());
    CHECK_EQ(r.count(), std::uint64_t{1});
    std::fclose(f);
}

// -------------------------------------------------------------- book building

TEST(builder_builds_and_validates_book_state) {
    Stream s;
    s.dir(7, "AAPL");
    s.dir(8, "MSFT");  // untracked locate: everything on it must be skipped
    s.sys(1 * SEC, 'Q');
    s.add(7, 2 * SEC, 1, 'B', 100, "AAPL", 1000000);   // bid 100.00
    s.add(7, 2 * SEC, 2, 'B', 50, "AAPL", 1000000);    // queued behind
    s.add(7, 2 * SEC, 3, 'S', 80, "AAPL", 1000100);    // ask 100.01
    s.add(8, 2 * SEC, 9, 'B', 10, "MSFT", 500000);     // untracked
    s.exec(7, 3 * SEC, 1, 40);                          // partial exec at the head
    s.cancel(7, 3 * SEC, 2, 10);                        // partial cancel keeps priority
    s.replace(7, 4 * SEC, 3, 4, 90, 1000200);           // ask moves to 100.02
    s.del(7, 5 * SEC, 4);
    BookBuilder b(one_symbol());
    run(s, b);

    const auto& bk = b.book(0);
    CHECK_EQ(bk.qty_at(ob::Side::Buy, 10000), std::uint64_t{100});  // 60 + 40
    CHECK(!bk.has_ask());                                            // replaced then deleted
    ob::OrderId head = 0;
    CHECK(bk.front_order(ob::Side::Buy, 10000, head));
    CHECK_EQ(head, ob::OrderId{1});  // partial exec + partial cancel both keep priority

    const auto& st = b.stats();
    CHECK_EQ(st.adds, std::uint64_t{4});  // 3 direct + 1 via replace
    CHECK_EQ(st.execs, std::uint64_t{1});
    CHECK_EQ(st.cancels, std::uint64_t{1});
    CHECK_EQ(st.replaces, std::uint64_t{1});
    CHECK_EQ(st.deletes, std::uint64_t{2});  // the U's delete half + the explicit D
    CHECK_EQ(st.unknown_ref, std::uint64_t{0});
    CHECK_EQ(st.rest_rejected, std::uint64_t{0});
    CHECK_EQ(st.qty_underflow, std::uint64_t{0});
    // front-of-queue fidelity: the one RTH exec hit the head of our best bid
    CHECK_EQ(st.e_rth, std::uint64_t{1});
    CHECK_EQ(st.e_front_best, std::uint64_t{1});
}

TEST(builder_counts_crossing_adds_only_in_market_hours) {
    Stream s;
    s.dir(7, "AAPL");
    s.add(7, 1 * SEC, 1, 'S', 100, "AAPL", 1000000);  // pre-open ask 100.00
    s.add(7, 2 * SEC, 2, 'B', 100, "AAPL", 1000100);  // pre-open bid 100.01: CROSSED, legal
    s.sys(3 * SEC, 'Q');
    s.add(7, 4 * SEC, 3, 'B', 10, "AAPL", 1000200);   // crossing add during market hours
    BookBuilder b(one_symbol());
    run(s, b);
    CHECK_EQ(b.stats().crossing_adds_rth, std::uint64_t{1});  // only the RTH one
    CHECK_EQ(b.stats().rest_rejected, std::uint64_t{0});      // crossed rests fine
    CHECK_EQ(b.book(0).best_bid(), ob::Price{10002});
    CHECK_EQ(b.book(0).best_ask(), ob::Price{10000});        // crossed book preserved
}

TEST(builder_ignores_subpenny_orders_and_their_later_events) {
    Stream s;
    s.dir(7, "AAPL");
    s.sys(1 * SEC, 'Q');
    s.add(7, 2 * SEC, 1, 'B', 100, "AAPL", 9990050);  // $999.0050: sub-penny -> skipped
    s.exec(7, 3 * SEC, 1, 40);                         // event on a skipped order
    s.replace(7, 4 * SEC, 1, 2, 50, 1000000);          // replacement chains the skip
    s.del(7, 5 * SEC, 2);
    BookBuilder b(one_symbol());
    run(s, b);
    const auto& st = b.stats();
    CHECK_EQ(st.subpenny_skipped, std::uint64_t{1});
    CHECK_EQ(st.ignored_events, std::uint64_t{3});  // E + U + D on the skipped chain
    CHECK_EQ(st.unknown_ref, std::uint64_t{0});     // NOT misread as feed anomalies
    CHECK_EQ(b.book(0).resting_orders(), std::size_t{0});
}

TEST(builder_counts_genuine_unknown_refs) {
    Stream s;
    s.dir(7, "AAPL");
    s.exec(7, 1 * SEC, 999, 10);  // never added
    BookBuilder b(one_symbol());
    run(s, b);
    CHECK_EQ(b.stats().unknown_ref, std::uint64_t{1});
}

// -------------------------------------------------------------- queue simulator

namespace {

// Standard scenario scaffold: AAPL open, bid queue [ref1: 100, ref2: 50] @ $100.00,
// ask 80 @ $100.01. QueueSim samples every 5s with a 60s horizon and 100sh phantoms.
struct SimRig {
    BookBuilder builder;
    QueueSim sim;
    SimRig(std::uint64_t horizon_s = 60)
        : builder(one_symbol()),
          sim(QueueSim::Config{100, 5 * SEC, horizon_s * SEC, ob::Side::Buy}, &builder) {
        builder.set_sink(&sim);
    }
    void feed(const Stream& s) { run(s, builder); }
};

void setup_queue(Stream& s) {
    s.dir(7, "AAPL");
    s.sys(10 * SEC, 'Q');
    s.add(7, 11 * SEC, 1, 'B', 100, "AAPL", 1000000);
    s.add(7, 11 * SEC, 2, 'B', 50, "AAPL", 1000000);
    s.add(7, 11 * SEC, 3, 'S', 80, "AAPL", 1000100);
    // sampling clock arms at the first market-open event (11s): first trial at >=16s
    s.add(7, 17 * SEC, 4, 'B', 10, "AAPL", 999900);  // any event past 16s opens the trial
}

}  // namespace

TEST(queue_sim_fills_after_queue_ahead_drains) {
    Stream s;
    setup_queue(s);
    // Drain the queue ahead: exec the head, delete the second.
    s.exec(7, 18 * SEC, 1, 100);
    s.del(7, 19 * SEC, 2);
    // A later order at our price executes: that volume is ours (we were in front).
    s.add(7, 20 * SEC, 5, 'B', 200, "AAPL", 1000000);
    s.exec(7, 21 * SEC, 5, 60);
    s.exec(7, 22 * SEC, 5, 40);
    SimRig rig;
    rig.feed(s);
    // Two trials: the 5s cadence samples again at the 21s exec. Sampling sees the book
    // AFTER that event applied (the builder notifies post-application), so the second
    // trial's queue is ref5's 200sh minus the 60 just executed. The one under test is
    // the first.
    CHECK_EQ(rig.sim.trials().size(), std::size_t{2});
    CHECK_EQ(rig.sim.trials()[1].ahead0, std::uint64_t{140});
    const auto& t = rig.sim.trials()[0];
    CHECK_EQ(t.ahead0, std::uint64_t{150});
    CHECK_EQ(t.norders0, std::uint32_t{2});
    CHECK_EQ(t.spread0, ob::Price{1});
    CHECK_EQ(t.remaining, std::uint32_t{0});   // 60 + 40 filled the 100sh phantom
    CHECK_EQ(t.t_first, 21 * SEC);
    CHECK_EQ(t.t_done, 22 * SEC);
    CHECK(!t.traded_through);
    CHECK_EQ(t.anomalies, std::uint32_t{0});
    CHECK(!t.open);
}

TEST(queue_sim_does_not_fill_while_queue_ahead_remains) {
    Stream s;
    setup_queue(s);
    s.exec(7, 18 * SEC, 1, 100);  // head gone, but ref2 (50sh) still ahead
    s.add(7, 20 * SEC, 5, 'B', 200, "AAPL", 1000000);
    s.exec(7, 21 * SEC, 5, 60);   // executes BEHIND us while 50sh ahead: anomaly, not a fill
    SimRig rig;
    rig.feed(s);
    const auto& t = rig.sim.trials()[0];
    CHECK_EQ(t.remaining, std::uint32_t{100});
    CHECK_EQ(t.anomalies, std::uint32_t{1});
}

TEST(queue_sim_partial_cancels_reduce_ahead_but_do_not_fill) {
    Stream s;
    setup_queue(s);
    s.cancel(7, 18 * SEC, 1, 100);  // X: full size off via partial cancel
    s.del(7, 19 * SEC, 2);
    SimRig rig;
    rig.feed(s);
    const auto& t = rig.sim.trials()[0];
    CHECK_EQ(t.ahead, std::uint64_t{0});      // queue drained...
    CHECK_EQ(t.remaining, std::uint32_t{100});  // ...but cancels trade nothing
    CHECK_EQ(t.t_first, std::uint64_t{0});
}

TEST(queue_sim_trade_through_fills_the_remainder) {
    Stream s;
    setup_queue(s);
    // An execution strictly below our price: the market traded through our level.
    s.exec(7, 18 * SEC, 4, 5);  // ref4 rests at 99.99 < our 100.00
    SimRig rig;
    rig.feed(s);
    const auto& t = rig.sim.trials()[0];
    CHECK(t.traded_through);
    CHECK_EQ(t.remaining, std::uint32_t{0});
    CHECK_EQ(t.t_first, 18 * SEC);
    CHECK(!t.open);
}

TEST(queue_sim_expires_at_horizon_without_late_fills) {
    Stream s;
    setup_queue(s);
    s.exec(7, 18 * SEC, 1, 100);
    s.del(7, 19 * SEC, 2);
    // Queue is empty, but the next execution at our price arrives AFTER the horizon:
    // the phantom was already cancelled and must NOT fill.
    s.add(7, 20 * SEC, 5, 'B', 200, "AAPL", 1000000);
    s.exec(7, 90 * SEC, 5, 60);  // trial opened at 17s, 10s horizon -> expired at 27s
    SimRig rig(/*horizon_s=*/10);
    rig.feed(s);
    const auto& t = rig.sim.trials()[0];
    CHECK_EQ(t.remaining, std::uint32_t{100});
    CHECK_EQ(t.t_done, 27 * SEC);  // t0 (17s) + horizon (10s)
    CHECK(!t.open);
}

int main() { return tf::run(); }
