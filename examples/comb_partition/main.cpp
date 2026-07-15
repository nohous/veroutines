/**
 * @brief Cross-boundary combinational settle - the dual-head cosim reaching a
 *        fixpoint on an acyclic comb path within one time slot.
 *
 * A combinational datapath is cut by the cosim boundary: the RTL half
 * (comb_slave.sv) reads a ROM and combines the result with a mask; the C++
 * half derives that mask from the ROM read. The comb path crosses the boundary
 * twice - addr (C++ -> RTL) -> rdata (RTL -> C++) -> mask (C++ -> RTL) ->
 * result (RTL -> C++) - with no clock in the path. Because it is acyclic, a
 * fixpoint exists; the delta loop relaxes to it in a handful of rounds, all
 * within the slot the input changed, so result carries zero added latency.
 * settled() reads the converged value; golden() computed entirely in C++ is
 * the oracle. A comb cycle here would instead trip the bounded-fixpoint guard
 * (CONTRACT.md C8) - convergence is only guaranteed for loop-free paths.
 */

#include "Vcomb_slave.h"
#include "verilated.h"

#include <veroutines/veroutines.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace veroutines;
using namespace veroutines::literals;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

namespace {

// The golden reference: the whole datapath computed in plain C++.
std::uint8_t rom(std::uint8_t a) {
    static const std::uint8_t m[16] = {
        0xA3, 0x17, 0xC5, 0x2E, 0x5B, 0x99, 0x04, 0xF0,
        0x3C, 0x81, 0x6D, 0xBE, 0x12, 0x47, 0xD8, 0x7A};
    return m[a & 0xF];
}

std::uint8_t rotl3(std::uint8_t d) { return std::uint8_t((d << 3) | (d >> 5)); }

std::uint8_t golden(std::uint8_t addr) {
    auto d = rom(addr);
    auto mask = rotl3(d);
    return std::uint8_t((d ^ mask) + addr);
}

struct rig {
    scheduler s;
    std::unique_ptr<VerilatedContext> ctx;
    std::unique_ptr<Vcomb_slave> top;

    model_in<std::uint8_t> addr, mask;
    model_out<std::uint8_t> rdata, result;
    bool trace = false;   // narrate the delta-round relaxation for one vector

    rig() {
        ctx = std::make_unique<VerilatedContext>();
        top = std::make_unique<Vcomb_slave>(ctx.get(), "");

        auto* t = top.get();
        addr.bind(s, [t](std::uint8_t v) { t->addr = v; });
        mask.bind(s, [t](std::uint8_t v) { t->mask = v; });
        rdata.bind(s, [t] { return t->rdata; });
        result.bind(s, [t] { return t->result; });

        // Pure combinational model: eval on demand, no internal timed events.
        s.eval_model = [this] { top->eval(); };
    }
};

// The C++ half of the datapath: a combinational process whose sensitivity list
// is changed(rdata) - process(all) in VHDL terms. It re-derives mask each time
// the RTL read settles, which is what carries the second boundary crossing.
task<> derive_mask(rig& r) {
    for (;;) {
        r.mask <<= rotl3(r.rdata);
        co_await changed(r.rdata);
    }
}

// A pure observer: logs each committed change of result. When tracing one
// vector it makes the delta rounds legible - several updates at the same
// simulated time as the comb chain relaxes to its fixpoint.
task<> watch_result(rig& r) {
    for (;;) {
        co_await changed(r.result);
        if (r.trace) {
            std::printf("    [t=%llu fs] result -> 0x%02X\n",
                        static_cast<unsigned long long>(r.s.now().count()), r.result.get());
        }
    }
}

// Drive every address in its own slot; the cross-boundary comb chain settles
// within that slot, so result is already golden by settled().
task<> sweep(rig& r, int& checked) {
    for (int a = 0; a < 256; a++) {
        r.trace = (a == 0x25);
        if (r.trace) {
            std::printf("  addr 0x24 -> 0x25, relaxing across delta rounds (same slot):\n");
        }

        auto t0 = r.s.now();
        r.addr <<= std::uint8_t(a);

        co_await settled();                       // the comb fixpoint for this slot

        CHECK(r.s.now() == t0);                   // resolved in the same slot: zero latency
        CHECK(r.result.get() == golden(std::uint8_t(a)));
        checked++;

        co_await 1_ns;                            // advance to the next slot
    }
    r.s.stop();
}

}

int main(int argc, char** argv) {
    rig r;
    r.ctx->commandArgs(argc, argv);

    // Optional unified VCD: +vcd or +vcd=<path>. The C++ model_in ports and the
    // model_out ports land in one file, both heads on one timeline.
    const char* vcd_path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (std::strncmp(argv[i], "+vcd=", 5) == 0) {
            vcd_path = argv[i] + 5;
        } else if (std::strcmp(argv[i], "+vcd") == 0) {
            vcd_path = "comb_partition.vcd";
        }
    }

    std::unique_ptr<wave> trace;
    if (vcd_path) {
        trace = std::make_unique<wave>(r.s, vcd_path, "tb");
        trace->scope("cpp");
        trace->add("addr", r.addr);
        trace->add("mask", r.mask);
        trace->pop();
        trace->scope("rtl");
        trace->add("rdata", r.rdata);
        trace->add("result", r.result);
        trace->pop();
    }

    int checked = 0;
    r.s.spawn(derive_mask(r), "derive_mask");
    r.s.spawn(watch_result(r), "watch_result");
    r.s.spawn(sweep(r, checked), "sweep");
    r.s.run();

    r.top->final();

    CHECK(checked == 256);

    // Show one worked vector so the boundary hops are legible.
    std::uint8_t a = 0x25;
    std::printf("  addr=0x%02X -> rdata=ROM[addr]=0x%02X -> mask=rotl3(rdata)=0x%02X"
                " -> result=(rdata^mask)+addr=0x%02X\n",
                a, rom(a), rotl3(rom(a)), golden(a));
    std::printf("  swept 256 addresses; every cross-boundary comb result matched golden\n");
    std::printf("  and settled within its own slot (zero added latency)\n");

    if (trace) {
        trace.reset();   // flush and close the VCD before reporting it
        std::printf("  wrote unified VCD (cpp model_in + rtl model_out) to %s\n", vcd_path);
    }

    std::printf("PASS comb_partition\n");
    return 0;
}
