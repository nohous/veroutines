/**
 * @brief UART link across the cosim boundary - the "feels at home" example.
 *
 * The transmitter is behavioral RTL (uart_tx.sv) on verilator; the receiver is
 * a C++ process that oversamples the serial line. One wire crosses the boundary
 * (txd, RTL -> C++). The receiver reads it like real hardware would: wait for
 * the start edge, step to the middle of each bit, sample DIV clocks apart, and
 * reassemble the byte (8N1, LSB first). No settled()/changed(any) here - just
 * edges, clocked sampling, and staged writes, so it reads like ordinary RTL.
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

constexpr int DIV = 8;   // clocks per bit; must match the RTL parameter

struct rig {
    scheduler s;
    std::unique_ptr<VerilatedContext> ctx;
    std::unique_ptr<Vuart_tx> top;

    drive<bool> clk, rst, start;
    drive<std::uint8_t> data;
    observed<bool> busy, txd;

    rig() {
        ctx = std::make_unique<VerilatedContext>();
        top = std::make_unique<Vuart_tx>(ctx.get(), "");

        auto* t = top.get();
        clk.bind(s, [t](bool v) { t->clk = v; });
        rst.bind(s, [t](bool v) { t->rst = v; });
        start.bind(s, [t](bool v) { t->start = v; });
        data.bind(s, [t](std::uint8_t v) { t->data = v; });
        busy.bind(s, [t] { return bool(t->busy); });
        txd.bind(s, [t] { return bool(t->txd); });

        s.eval_model = [this] { top->eval(); };
    }
};

task<> gen_clock(rig& r) {
    for (;;) {
        co_await 5_ns;
        r.clk <<= !r.clk;
    }
}

// Transmit each byte: pulse start when the line is free, wait out the frame.
task<> tx_driver(rig& r, const std::vector<std::uint8_t>& msg) {
    r.rst <<= true;
    co_await posedge(r.clk);
    r.rst <<= false;

    for (auto byte : msg) {
        co_await posedge(r.clk);
        while (r.busy) {
            co_await posedge(r.clk);
        }

        r.data <<= byte;
        r.start <<= true;
        co_await posedge(r.clk);
        r.start <<= false;

        co_await posedge(r.clk);
        while (r.busy) {
            co_await posedge(r.clk);
        }
    }
}

task<> wait_clocks(rig& r, int n) {
    for (int i = 0; i < n; i++) {
        co_await posedge(r.clk);
    }
}

// Oversampling receiver: recover bytes from txd using only clk and the line.
task<> rx(rig& r, std::vector<std::uint8_t>& got, int want) {
    while (int(got.size()) < want) {
        do {
            co_await posedge(r.clk);
        } while (r.txd);                          // hunt for the start bit (line low)

        co_await wait_clocks(r, DIV / 2);         // step to the middle of the start bit
        CHECK(!r.txd);                            // still low at its centre

        std::uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            co_await wait_clocks(r, DIV);         // advance to the next bit centre
            if (r.txd) {
                byte |= std::uint8_t(1 << b);
            }
        }

        co_await wait_clocks(r, DIV);             // centre of the stop bit
        CHECK(r.txd);                             // stop bit must be high (framing)

        got.push_back(byte);
    }
}

task<> watchdog(rig& r) {
    co_await 100_us;
    std::fprintf(stderr, "uart_link: watchdog fired\n");
    std::exit(1);
}

}

int main(int argc, char** argv) {
    rig r;
    r.ctx->commandArgs(argc, argv);

    const std::vector<std::uint8_t> msg = {0x55, 0xAA, 0x42, 0x3C, 0xF0, 'O', 'k'};
    std::vector<std::uint8_t> got;

    r.s.spawn(gen_clock(r), "clk");
    r.s.spawn(tx_driver(r, msg), "tx");
    r.s.spawn(rx(r, got, int(msg.size())), "rx");
    r.s.spawn(watchdog(r), "watchdog");
    r.s.run_until(100_us);

    r.top->final();

    CHECK(got.size() == msg.size());
    for (std::size_t i = 0; i < msg.size(); i++) {
        CHECK(got[i] == msg[i]);
    }

    std::printf("  sent    :");
    for (auto b : msg) { std::printf(" 0x%02X", b); }
    std::printf("\n  received:");
    for (auto b : got) { std::printf(" 0x%02X", b); }
    std::printf("\n  %zu bytes recovered from the serial line, 8N1 at clk/%d\n", got.size(), DIV);
    std::printf("PASS uart_link\n");
    return 0;
}
