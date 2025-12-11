#include <format>
#include <iostream>
#include <verilated.h>
#include "verilated_vcd_c.h"
#include "Vaxibox.h"
#include "veroutines.h"

using std::cout;
using std::format;
using namespace Veroutines;

int main(int argc, char** argv, char**) {
    // Setup
    Verilated::mkdir("logs");
    Verilated::debug(0);
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    const auto tfp = std::make_unique<VerilatedVcdC>();
    const std::unique_ptr<Vaxibox> top{new Vaxibox{contextp.get(), ""}};

    contextp->randReset(2);
    contextp->traceEverOn(true);
    contextp->commandArgs(argc, argv);

    top->trace(tfp.get(), 99);
    tfp->open("logs/dump.vcd");
    if (!tfp->isOpen()) {
        cout << "trace not open\n";
        return EXIT_FAILURE;
    }

    Scheduler sched;

    // ========================================================================
    // Register boundary ports
    // ========================================================================

    // Inputs (Testbench -> DUT)
    auto clk      = sched.input(&top->clk);
    auto rst      = sched.input(&top->rst);
    auto s_tvalid = sched.input(&top->s_tvalid);
    auto s_tdata  = sched.input(&top->s_tdata);
    auto m_tready = sched.input(&top->m_tready);

    // Outputs (DUT -> Testbench)
    auto s_tready = sched.output(&top->s_tready);
    auto m_tvalid = sched.output(&top->m_tvalid);
    auto m_tdata  = sched.output(&top->m_tdata);
    auto event_out = sched.output(&top->event_out);

    // ========================================================================
    // Clock Generator (using timed events)
    // ========================================================================

    std::function<void()> run_clk = [&]() {
        clk->write(!clk->val());
        sched.schedule_after(5, run_clk);  // 10ns period
    };
    run_clk();

    // ========================================================================
    // Reset Sequence
    // ========================================================================

    sched.schedule_after(1,  [&]() { rst->write(1); });
    sched.schedule_after(20, [&]() { rst->write(0); });

    // ========================================================================
    // AXI Stream Driver (sensitive to clock + reset)
    // ========================================================================

    uint8_t data_to_send = 0;

    sched.process({clk, rst}, [&](Scheduler& s) {
        if (clk->posedge() && !rst->val()) {
            // Check flow control (s_tready is OUTPUT from DUT)
            if (s_tready->val()) {
                if (data_to_send < 16) {
                    s_tvalid->write(1);
                    s_tdata->write(data_to_send);
                    cout << format("@{:4d} DRV: Sent 0x{:02X}\n", s.time(), data_to_send);
                    data_to_send++;
                } else {
                    s_tvalid->write(0);
                    s_tdata->write(0);
                }
            }
        }
    });

    // ========================================================================
    // Monitor (sensitive to clock)
    // ========================================================================

    // Set m_tready = 1 permanently for this test
    m_tready->write(1);

    sched.process({clk}, [&](Scheduler& s) {
        if (clk->posedge() && m_tvalid->val() && m_tready->val()) {
            uint8_t actual = m_tdata->val();
            cout << format("@{:4d} MON: Got  0x{:02X} (Reversed)\n", s.time(), actual);
        }
    });

    // ========================================================================
    // Event Listener (sensitive to async event output)
    // ========================================================================

    sched.process({event_out}, [&](Scheduler& s) {
        if (event_out->posedge()) {
            cout << format("\n!!! @{:4d} EVENT DETECTED: Counter reached 3 !!!\n\n", s.time());
        }
    });

    // ========================================================================
    // Run Simulation
    // ========================================================================

    cout << "Starting Simulation...\n";
    sched.run(contextp.get(), top.get(), tfp.get(), 500);

    cout << "Simulation Finished.\n";

    if (data_to_send == 16) cout << "SUCCESS: All data sent.\n";
    else cout << "FAILURE: Timed out before sending all data.\n";

    // Cleanup
    top->final();
    tfp->close();
    contextp->statsPrintSummary();

    return 0;
}
