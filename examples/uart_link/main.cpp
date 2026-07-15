/**
 * @brief UART link, structured to show the behav/sim split - and to run the
 *        same testbench against a C++ behavioral model and the verilated RTL.
 *
 * Two layers, told apart by vocabulary:
 *   - behav: uart_tx_model is a pure model - signal + process + comb + edges,
 *     nothing else. It is a direct transcription of uart_tx.sv, and uses no
 *     model_in/model_out, no settled()/changed(any), no eval_model.
 *   - sim: the clock, the stimulus, the oversampling receiver and the checker.
 *     They are templated over a port set, so the exact same code drives either
 *     the C++ model (ports are signal) or the verilated RTL (ports are
 *     model_in/model_out).
 *
 * main runs both backends on the same message and asserts they recover it
 * identically - so the testbench is backend-agnostic and the C++ model is
 * cross-validated against the RTL.
 */

#include "Vuart_tx.h"
#include "verilated.h"

#include <veroutines/veroutines.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

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

constexpr int DIV = 8;   // clocks per bit; matches the RTL parameter

// -----------------------------------------------------------------------------
// behav: the model - everyday vocabulary only (signal, process, comb, edges)
// -----------------------------------------------------------------------------

// A transcription of uart_tx.sv: 8N1, LSB first, one bit every DIV clocks. Its
// ports are plain signals connected by the harness; it drives busy and txd.
struct uart_tx_model {
    signal<bool>&         clk;
    signal<bool>&         rst;
    signal<std::uint8_t>& data;
    signal<bool>&         start;
    signal<bool>&         busy;
    signal<bool>&         txd;

    signal<std::uint16_t> shifter;
    signal<int>           bitcnt;
    signal<int>           divcnt;
    signal<bool>          sending;

    uart_tx_model(scheduler& s, signal<bool>& clk_, signal<bool>& rst_,
                  signal<std::uint8_t>& data_, signal<bool>& start_,
                  signal<bool>& busy_, signal<bool>& txd_)
        : clk(clk_), rst(rst_), data(data_), start(start_), busy(busy_), txd(txd_),
          shifter{s, 0x3FF}, bitcnt{s, 0}, divcnt{s, 0}, sending{s, false} {
        comb(s, [this] { busy <<= sending; });
        comb(s, [this] { txd <<= sending ? bool(shifter.get() & 1) : true; });
        process(s, rising_edge(clk), [this] { step(); });
    }

    void step() {
        if (rst) {
            sending <<= false;
            bitcnt  <<= 0;
            divcnt  <<= 0;
            shifter <<= std::uint16_t(0x3FF);
        } else if (!sending) {
            if (start) {
                sending <<= true;
                shifter <<= std::uint16_t((1u << 9) | (std::uint16_t(data.get()) << 1));
                bitcnt  <<= 0;
                divcnt  <<= 0;
            }
        } else if (divcnt == DIV - 1) {
            divcnt  <<= 0;
            shifter <<= std::uint16_t((1u << 9) | (shifter.get() >> 1));
            if (bitcnt == 9) {
                sending <<= false;
            } else {
                bitcnt <<= bitcnt + 1;
            }
        } else {
            divcnt <<= divcnt + 1;
        }
    }
};

// -----------------------------------------------------------------------------
// sim: the testbench - templated over the port set (signal or model_in/out)
// -----------------------------------------------------------------------------

struct cpp_ports {
    signal<bool>&         clk;
    signal<bool>&         rst;
    signal<std::uint8_t>& data;
    signal<bool>&         start;
    signal<bool>&         busy;
    signal<bool>&         txd;
};

struct rtl_ports {
    model_in<bool>&         clk;
    model_in<bool>&         rst;
    model_in<std::uint8_t>& data;
    model_in<bool>&         start;
    model_out<bool>&        busy;
    model_out<bool>&        txd;
};

template<typename Ports>
task<> gen_clock(Ports& p) {
    for (;;) {
        co_await 5_ns;
        p.clk <<= !p.clk;
    }
}

template<typename Ports>
task<> tx_driver(Ports& p, const std::vector<std::uint8_t>& msg) {
    p.rst <<= true;
    co_await posedge(p.clk);
    p.rst <<= false;

    for (auto byte : msg) {
        co_await posedge(p.clk);
        while (p.busy) {
            co_await posedge(p.clk);
        }

        p.data <<= byte;
        p.start <<= true;
        co_await posedge(p.clk);
        p.start <<= false;

        co_await posedge(p.clk);
        while (p.busy) {
            co_await posedge(p.clk);
        }
    }
}

template<typename Ports>
task<> wait_clocks(Ports& p, int n) {
    for (int i = 0; i < n; i++) {
        co_await posedge(p.clk);
    }
}

template<typename Ports>
task<> rx(Ports& p, std::vector<std::uint8_t>& got, int want) {
    while (int(got.size()) < want) {
        do {
            co_await posedge(p.clk);
        } while (p.txd);                          // hunt for the start bit (line low)

        co_await wait_clocks(p, DIV / 2);         // step to the middle of the start bit
        CHECK(!p.txd);

        std::uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            co_await wait_clocks(p, DIV);
            if (p.txd) {
                byte |= std::uint8_t(1 << b);
            }
        }

        co_await wait_clocks(p, DIV);             // centre of the stop bit
        CHECK(p.txd);

        got.push_back(byte);
    }
}

// -----------------------------------------------------------------------------
// Backends: same testbench, C++ model on one side, verilated RTL on the other
// -----------------------------------------------------------------------------

std::vector<std::uint8_t> run_cpp(const std::vector<std::uint8_t>& msg) {
    scheduler s;
    signal<bool> clk{s}, rst{s}, start{s}, busy{s}, txd{s, true};
    signal<std::uint8_t> data{s};

    uart_tx_model dut(s, clk, rst, data, start, busy, txd);
    cpp_ports p{clk, rst, data, start, busy, txd};

    std::vector<std::uint8_t> got;
    s.spawn(gen_clock(p), "clk");
    s.spawn(tx_driver(p, msg), "tx");
    s.spawn(rx(p, got, int(msg.size())), "rx");
    s.run_until(100_us);
    return got;
}

std::vector<std::uint8_t> run_rtl(int argc, char** argv, const std::vector<std::uint8_t>& msg) {
    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(argc, argv);
    auto top = std::make_unique<Vuart_tx>(ctx.get(), "");

    scheduler s;
    model_in<bool> clk, rst, start;
    model_in<std::uint8_t> data;
    model_out<bool> busy, txd;

    auto* t = top.get();
    clk.bind(s, [t](bool v) { t->clk = v; });
    rst.bind(s, [t](bool v) { t->rst = v; });
    start.bind(s, [t](bool v) { t->start = v; });
    data.bind(s, [t](std::uint8_t v) { t->data = v; });
    busy.bind(s, [t] { return bool(t->busy); });
    txd.bind(s, [t] { return bool(t->txd); });
    s.eval_model = [&] { top->eval(); };

    rtl_ports p{clk, rst, data, start, busy, txd};

    std::vector<std::uint8_t> got;
    s.spawn(gen_clock(p), "clk");
    s.spawn(tx_driver(p, msg), "tx");
    s.spawn(rx(p, got, int(msg.size())), "rx");
    s.run_until(100_us);

    top->final();
    return got;
}

}

int main(int argc, char** argv) {
    const std::vector<std::uint8_t> msg = {0x55, 0xAA, 0x42, 0x3C, 0xF0, 'O', 'k'};

    auto got_cpp = run_cpp(msg);
    auto got_rtl = run_rtl(argc, argv, msg);

    CHECK(got_cpp == msg);            // the C++ model transmits the message
    CHECK(got_rtl == msg);            // so does the verilated RTL
    CHECK(got_cpp == got_rtl);        // and they agree, bit for bit

    std::printf("  sent    :");
    for (auto b : msg) { std::printf(" 0x%02X", b); }
    std::printf("\n  C++ rx  :");
    for (auto b : got_cpp) { std::printf(" 0x%02X", b); }
    std::printf("\n  RTL rx  :");
    for (auto b : got_rtl) { std::printf(" 0x%02X", b); }
    std::printf("\n  one testbench drove both heads; C++ model and RTL agreed on %zu bytes\n",
                got_cpp.size());
    std::printf("PASS uart_link\n");
    return 0;
}
