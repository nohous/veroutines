/**
 * @brief Tier-1 cosim interlock: veroutines processes against t1_top.sv.
 *
 * Pins the cosim boundary semantics on real verilated RTL:
 * - flop: comb outputs land in the driving slot; a flop takes a drive at
 *   the next edge; a drive co-staged with the edge is sampled by it, a
 *   drive staged after waking on the edge is not (next edge).
 * - echo: multi-cycle FSM handshake, exact 35 ns latency every round trip.
 * - svmaster: SV initial-block master with pure delays drives the C++
 *   reactive side; wake times come from model_next_event alone.
 * - counters: coprime clocks (10/7 ns), exact posedge counts after 140 ns.
 */

#include "Vt1_top.h"
#include "verilated.h"
#include "../t0.h"

#include <memory>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

struct rig {
    scheduler s;
    std::unique_ptr<VerilatedContext> ctx;
    std::unique_ptr<Vt1_top> top;

    drive<bool> clk, clk2, f_d, e_req, m_ack;
    drive<std::uint8_t> e_data;
    observed<bool> f_q, f_comb, e_ack, m_req;
    observed<std::uint8_t> e_echo, m_data;
    observed<std::uint16_t> c1, c2;

    bool done_flop = false, done_echo = false, done_svm = false, done_clocks = false;

    rig(int argc, char** argv) {
        ctx = std::make_unique<VerilatedContext>();
        ctx->commandArgs(argc, argv);
        top = std::make_unique<Vt1_top>(ctx.get(), "");

        auto* t = top.get();
        clk.bind(s, [t](bool v) { t->clk = v; });
        clk2.bind(s, [t](bool v) { t->clk2 = v; });
        f_d.bind(s, [t](bool v) { t->f_d = v; });
        e_req.bind(s, [t](bool v) { t->e_req = v; });
        m_ack.bind(s, [t](bool v) { t->m_ack = v; });
        e_data.bind(s, [t](std::uint8_t v) { t->e_data = v; });

        f_q.bind(s, [t] { return bool(t->f_q); });
        f_comb.bind(s, [t] { return bool(t->f_comb); });
        e_ack.bind(s, [t] { return bool(t->e_ack); });
        m_req.bind(s, [t] { return bool(t->m_req); });
        e_echo.bind(s, [t] { return t->e_echo; });
        m_data.bind(s, [t] { return t->m_data; });
        c1.bind(s, [t] { return t->c1_count; });
        c2.bind(s, [t] { return t->c2_count; });

        s.eval_model = [this] { top->eval(); };
        s.model_next_event = [this]() -> std::uint64_t {
            return top->eventsPending() ? std::uint64_t(top->nextTimeSlot()) : never;
        };
        s.on_time_advance = [this](std::uint64_t tt) { ctx->time(tt); };
    }
};

task<> gen_clock(drive<bool>& c, sim_time half) {
    for (;;) {
        co_await half;
        c <<= !c;
    }
}

// posedges of clk at 5 + 10k ns, negedges at 10 + 10k ns.
task<> flop_test(rig& r) {
    co_await 12_ns;                       // between edges
    r.f_d <<= 1;
    co_await changed(r.f_comb);
    CHECK(r.s.now() == 12_ns);            // comb passthrough lands in the driving slot
    CHECK(r.f_q.get() == 0);

    co_await changed(r.f_q);
    CHECK(r.s.now() == 15_ns);            // the flop takes it at the next posedge
    CHECK(r.f_q.get() == 1);

    co_await 10_ns;                       // wake at 25 ns, the same slot as a rise
    r.f_d <<= 0;                          // co-staged with the edge: the flop samples it
    co_await changed(r.f_q);
    CHECK(r.s.now() == 25_ns);
    CHECK(r.f_q.get() == 0);

    co_await posedge(r.clk);              // wake at 35 ns, the round after the edge eval
    r.f_d <<= 1;                          // lands after the edge: not sampled until 45
    co_await changed(r.f_q);
    CHECK(r.s.now() == 45_ns);
    CHECK(r.f_q.get() == 1);

    println("  flop: comb same-slot at 12 ns, flop at 15 ns;"
            " co-staged race taken at 25 ns, post-wake drive deferred to 45 ns");
    r.done_flop = true;
}

// Drive at a negedge; the FSM samples the toggle at the next posedge
// (+5 ns) and toggles ack 3 posedges later (+30 ns): 35 ns round trip.
task<> echo_test(rig& r) {
    for (auto i = 0; i < 5; i++) {
        co_await negedge(r.clk);
        auto t0 = r.s.now();
        r.e_data <<= std::uint8_t(10 + i);
        r.e_req <<= !r.e_req;

        co_await changed(r.e_ack);
        CHECK(r.s.now() - t0 == 35_ns);
        CHECK(r.e_echo.get() == 10 + i);
    }

    println("  echo: 5 handshakes through the FSM, 35 ns latency each, payloads match");
    r.done_echo = true;
}

// The SV initial block toggles m_req at 23 + 40k ns with data = 3(k+1);
// nothing on the cosim side is scheduled at those times - the wakes come
// from model_next_event alone.
task<> svmaster_test(rig& r) {
    for (auto i = 1; i <= 5; i++) {
        co_await changed(r.m_req);
        CHECK(r.s.now() == 23_ns + sim_time{std::uint64_t(i - 1) * 40'000'000ull});
        CHECK(r.m_data.get() == std::uint8_t(3 * i));
        r.m_ack <<= !r.m_ack;
    }

    println("  svmaster: 5 toggles observed at 23 + 40k ns, SV-side delays drove the clockless wakes");
    r.done_svm = true;
}

// clk rises at 5 + 10k, clk2 rises at 3.5 + 7k; by 140 ns that is 14 and
// 20 posedges, with toggles of both clocks colliding in one slot at 35,
// 70, 105 and 140 ns.
task<> clocks_test(rig& r) {
    co_await 140_ns;
    CHECK(r.c1.get() == 14);
    CHECK(r.c2.get() == 20);

    println("  counters: c1={} c2={} after 140 ns of coprime 10/7 ns clocks", r.c1.get(), r.c2.get());
    r.done_clocks = true;
}

task<> supervisor(rig& r) {
    co_await until([&r] {
        return r.done_flop && r.done_echo && r.done_svm && r.done_clocks;
    });
    r.s.stop();
}

task<> watchdog(rig& r) {
    co_await 10_us;
    std::fprintf(stderr, "t1: watchdog fired: flop=%d echo=%d svm=%d clocks=%d\n",
                 int(r.done_flop), int(r.done_echo), int(r.done_svm), int(r.done_clocks));
    std::exit(1);
}

}

int main(int argc, char** argv) {
    rig r(argc, argv);

    r.s.spawn(gen_clock(r.clk, 5_ns), "clk");
    r.s.spawn(gen_clock(r.clk2, 3500_ps), "clk2");
    r.s.spawn(flop_test(r), "flop_test");
    r.s.spawn(echo_test(r), "echo_test");
    r.s.spawn(svmaster_test(r), "svmaster_test");
    r.s.spawn(clocks_test(r), "clocks_test");
    r.s.spawn(supervisor(r), "supervisor");
    r.s.spawn(watchdog(r), "watchdog");

    r.s.run();
    r.top->final();

    CHECK(r.done_flop && r.done_echo && r.done_svm && r.done_clocks);
    return t0_pass("t1_cosim");
}
