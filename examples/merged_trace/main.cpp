/**
 * @brief Full-hierarchy cosim trace: emit two VCDs on one time axis and merge.
 *
 * The single-file wave tracer can only see model signals you surface through
 * model_out. To get the verilated model's *entire* hierarchy - every internal,
 * for free - alongside the C++ signals, let verilator's own tracer dump the
 * model (rtl.vcd) and let veroutines' wave dump the C++ side (cpp.vcd), then
 * merge them (scripts/merge_vcd.py). The two share a time axis because
 * on_time_advance stamps the model clock with veroutines' time, so the merge is
 * mechanical. This is the "post-process starter": full internals with one extra
 * command; the model_out probe path is the no-merge, curated-internals mode.
 */

#include "Vuart_tx.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <veroutines/veroutines.h>

#include <cstdio>
#include <memory>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

constexpr int DIV = 8;

task<> clkgen(model_in<bool>& clk) {
    for (;;) { co_await 5_ns; clk <<= !clk; }
}

task<> txdrv(model_in<bool>& clk, model_in<bool>& rst, model_in<std::uint8_t>& data,
             model_in<bool>& start, model_out<bool>& busy, scheduler& s) {
    rst <<= true;  co_await posedge(clk); rst <<= false;
    co_await posedge(clk);
    data <<= 0x55; start <<= true; co_await posedge(clk); start <<= false;
    co_await posedge(clk);
    while (busy) { co_await posedge(clk); }
    co_await 200_ns;
    s.stop();
}

// The C++-side state the verilated model cannot see: the receiver's progress.
task<> rxwatch(model_in<bool>& clk, model_out<bool>& txd,
               signal<std::uint8_t>& seen, signal<int>& bitpos) {
    for (;;) {
        do { co_await posedge(clk); } while (txd);
        for (int i = 0; i < DIV / 2; i++) { co_await posedge(clk); }
        std::uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            for (int i = 0; i < DIV; i++) { co_await posedge(clk); }
            if (txd) { b |= std::uint8_t(1 << k); }
            bitpos <<= k;
            seen <<= b;
        }
        for (int i = 0; i < DIV; i++) { co_await posedge(clk); }
    }
}

// Dump the verilated model's full-hierarchy trace once per settled slot, in
// step with veroutines' own settled-phase wave dump.
task<> rtl_pump(VerilatedVcdC& tfp, VerilatedContext& ctx) {
    for (;;) { co_await settled(); tfp.dump(ctx.time()); }
}

bool nonempty(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { return false; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fclose(f);
    return n > 0;
}

}

int main(int argc, char** argv) {
    const char* rtl_path = "merged_trace.rtl.vcd";
    const char* cpp_path = "merged_trace.cpp.vcd";

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(argc, argv);
    auto top = std::make_unique<Vuart_tx>(ctx.get(), "");

    scheduler s;
    model_in<bool> clk, rst, start;
    model_in<std::uint8_t> data;
    model_out<bool> busy, txd;

    auto* t = top.get();
    clk.bind(s,   [t](bool v) { t->clk = v; });
    rst.bind(s,   [t](bool v) { t->rst = v; });
    start.bind(s, [t](bool v) { t->start = v; });
    data.bind(s,  [t](std::uint8_t v) { t->data = v; });
    busy.bind(s,  [t] { return bool(t->busy); });
    txd.bind(s,   [t] { return bool(t->txd); });
    s.eval_model = [&] { top->eval(); };
    s.on_time_advance = [&](std::uint64_t tt) { ctx->time(tt); };   // one shared time axis

    signal<std::uint8_t> seen{s};
    signal<int> bitpos{s};

    auto w = std::make_unique<wave>(s, cpp_path, "tb");
    w->scope("cpp_rx");
    w->add("seen", seen);
    w->add("bitpos", bitpos, 4);
    w->pop();

    Verilated::traceEverOn(true);
    VerilatedVcdC tfp;
    top->trace(&tfp, 99);
    tfp.set_time_unit("1fs");
    tfp.set_time_resolution("1fs");
    tfp.open(rtl_path);

    s.spawn(clkgen(clk), "clk");
    s.spawn(txdrv(clk, rst, data, start, busy, s), "drv");
    s.spawn(rxwatch(clk, txd, seen, bitpos), "rx");
    s.spawn(rtl_pump(tfp, *ctx), "rtl_vcd");
    s.run();

    tfp.close();
    w.reset();          // flush and close the C++ VCD before reading it back
    top->final();

    if (!nonempty(rtl_path) || !nonempty(cpp_path)) {
        std::fprintf(stderr, "FAIL: a VCD was not written\n");
        return 1;
    }

    std::printf("  wrote %s (full RTL hierarchy) and %s (C++ signals)\n", rtl_path, cpp_path);
    std::printf("  merge:  python3 scripts/merge_vcd.py merged.vcd %s %s\n", rtl_path, cpp_path);
    std::printf("  then open merged.vcd in gtkwave - RTL internals and C++ signals, one file\n");
    std::printf("PASS merged_trace\n");
    return 0;
}
