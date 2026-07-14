/**
 * @brief C4: one wake per edge; a wait observes only changes committed
 *        after it was armed; rose()/fell() hold through the waking round.
 */

#include "t0.h"

#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

task<> gen_clock(signal<bool>& clk, int halves) {
    for (int i = 0; i < halves; i++) {
        co_await 5_ns;   // durations await directly (C9)
        clk <<= !clk;
    }
}

task<> count_rises(scheduler& s, signal<bool>& clk, std::vector<sim_time>& at) {
    for (;;) {
        co_await posedge(clk);

        CHECK(clk.rose());
        CHECK(!clk.fell());
        at.push_back(s.now());
    }
}

// Arms posedge between two rises: wakes at the next rise; the rise
// already in the past stays unseen (generation captured at arm time).
task<> late_armed(scheduler& s, signal<bool>& clk, sim_time& woke) {
    co_await delay(12_ns);
    co_await posedge(clk);
    woke = s.now();
}

// Same-slot delta edge: the flag rises in round 0 of the 7 ns slot; the
// waiter wakes one round later, still at 7 ns.
task<> pulse(signal<bool>& flag) {
    co_await delay(7_ns);
    flag <<= true;
}

task<> watch(scheduler& s, signal<bool>& flag, sim_time& woke) {
    co_await posedge(flag);
    woke = s.now();
}

}

int main() {
    scheduler s;
    signal<bool> clk{s};
    signal<bool> flag{s};
    std::vector<sim_time> rises;
    sim_time late_woke{};
    sim_time flag_woke{};

    s.spawn(gen_clock(clk, 6));            // rises at 5, 15, 25 ns
    s.spawn(count_rises(s, clk, rises));
    s.spawn(late_armed(s, clk, late_woke));
    s.spawn(pulse(flag));
    s.spawn(watch(s, flag, flag_woke));
    s.run();

    CHECK(rises.size() == 3);
    CHECK(rises[0] == 5_ns);
    CHECK(rises[1] == 15_ns);
    CHECK(rises[2] == 25_ns);
    CHECK(late_woke == 15_ns);
    CHECK(flag_woke == 7_ns);

    println("  rises seen at {} / {} / {} ns, each exactly once",
            rises[0].count() / 1e6, rises[1].count() / 1e6, rises[2].count() / 1e6);
    println("  armed at 12 ns -> woke at {} ns (the 5 ns rise stayed unseen)",
            late_woke.count() / 1e6);
    println("  same-slot delta edge woke at {} ns", flag_woke.count() / 1e6);
    return t0_pass("t0_edges");
}
