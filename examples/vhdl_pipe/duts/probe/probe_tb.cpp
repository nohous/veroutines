/**
 * @brief Drive the verilated probe under veroutines and dump its full RTL
 *        hierarchy to an FST.
 *
 * Deliberately dumb: clock, a reset pulse, then walk a/b through their four
 * combinations so every downstream net toggles. The point is not behaviour but
 * a wave whose signal tree we read back - whatever survived ghdl-synth appears
 * in dut.fst, whatever was optimised away does not. verilator's own tracer
 * emits the whole hierarchy; veroutines owns the clock and the shared time axis.
 */

#include "Vdut.h"
#include "verilated.h"
#include "verilated_fst_c.h"

#include <veroutines/veroutines.h>

#include <cstdio>
#include <memory>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

task<> clkgen(model_in<bool>& clk) {
    for (;;) {
        co_await 5_ns;
        clk <<= !clk;
    }
}

task<> stim(model_in<bool>& rst, model_in<bool>& a, model_in<bool>& b,
            model_in<bool>& clk, scheduler& s) {
    rst <<= true;
    a <<= false;
    b <<= false;
    co_await posedge(clk);
    co_await posedge(clk);
    rst <<= false;

    for (int i = 0; i < 16; i++) {
        a <<= bool(i & 1);
        b <<= bool(i & 2);
        co_await posedge(clk);
    }

    co_await 20_ns;
    s.stop();
}

}

int main(int argc, char** argv) {
    const char* fst_path = "dut.fst";

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(argc, argv);
    auto top = std::make_unique<Vdut>(ctx.get(), "");

    scheduler s;
    model_in<bool> clk, rst, a, b;
    model_out<std::uint8_t> count;
    model_out<bool> parity, leafq;

    auto* t = top.get();
    clk.bind(s, [t](bool v) { t->clk = v; });
    rst.bind(s, [t](bool v) { t->rst = v; });
    a.bind(s,   [t](bool v) { t->a = v; });
    b.bind(s,   [t](bool v) { t->b = v; });
    count.bind(s,  [t] { return std::uint8_t(t->count); });
    parity.bind(s, [t] { return bool(t->parity); });
    leafq.bind(s,  [t] { return bool(t->leafq); });

    s.eval_model = [&] { top->eval(); };
    s.on_time_advance = [&](std::uint64_t tt) { ctx->time(tt); };

    Verilated::traceEverOn(true);
    VerilatedFstC tfp;
    top->trace(&tfp, 99);
    tfp.open(fst_path);

    s.spawn(clkgen(clk), "clk");
    s.spawn(stim(rst, a, b, clk, s), "stim");
    s.spawn([&]() -> task<> {
        for (;;) {
            co_await settled();
            tfp.dump(ctx->time());
        }
    }(), "fst");

    s.run();

    tfp.close();
    top->final();

    std::printf("PASS probe_tb: wrote %s\n", fst_path);
    return 0;
}
