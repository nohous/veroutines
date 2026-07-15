/**
 * @brief HDL surface: process(rising_edge(clk)) and comb() build a small design
 *        - a clocked counter with a combinational output - from the sugar. The
 *        combinational statement carries no sensitivity list; it converges by
 *        the always-eval fixpoint (C5).
 */

#include "t0.h"

using namespace veroutines;
using namespace veroutines::literals;

namespace {

// An "architecture body": the constructor declares the processes, VHDL-style.
struct dut {
    scheduler s;
    signal<bool> clk{s};
    signal<int> count{s, 0};
    signal<int> doubled{s, 0};
    signal<bool> is_five{s, false};

    dut() {
        process(s, rising_edge(clk), [&] { count <<= count + 1; }, "count");
        comb(s, [&] { doubled <<= count * 2; }, "doubled");
        comb(s, [&] { is_five <<= (count == 5); }, "is_five");
    }
};

task<> gen_clock(signal<bool>& clk, int halves) {
    for (int i = 0; i < halves; i++) {
        co_await 5_ns;
        clk <<= !clk;
    }
}

// The comb output settles one delta round after count commits, so the invariant
// is read at settled() - the slot's converged state, not the input's edge round.
task<> check_comb(dut& d) {
    for (;;) {
        co_await settled();
        CHECK(d.doubled == d.count * 2);
        CHECK(d.is_five == (d.count == 5));
    }
}

}

int main() {
    dut d;
    d.s.spawn(gen_clock(d.clk, 20), "clk");   // rises at 5, 15, ... 95 ns -> 10 rises
    d.s.spawn(check_comb(d), "checker");
    d.s.run();

    CHECK(d.count.get() == 10);
    CHECK(d.doubled.get() == 20);
    CHECK(d.is_five.get() == false);

    println("  process(rising_edge(clk)) counted {} rises", d.count.get());
    println("  comb: doubled = {}, tracked count*2 every cycle with no sensitivity list",
            d.doubled.get());
    return t0_pass("t0_hdl");
}
