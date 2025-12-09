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
    tfp->dump(0);

    Scheduler sched;

    auto clk      = sched.register_signal(&top->clk);
    auto rst      = sched.register_signal(&top->rst);
    
    auto s_tvalid = sched.register_signal(&top->s_tvalid);
    auto s_tready = sched.register_signal(&top->s_tready);
    auto s_tdata  = sched.register_signal(&top->s_tdata);
    
    auto m_tvalid = sched.register_signal(&top->m_tvalid);
    auto m_tready = sched.register_signal(&top->m_tready);
    auto m_tdata  = sched.register_signal(&top->m_tdata);
    
    auto event_out = sched.register_signal(&top->event_out);

    // 3. Define Behaviors

    // --- Clock Generator ---
    std::function<void()> run_clk = [&]() {
        sched.schedule_write(&top->clk, (uint8_t)!top->clk);
        sched.schedule_after(5, run_clk); // 10ns period
    };
    run_clk(); // Start it

    // --- Reset Sequence ---
    sched.schedule_after(1,  [&]() { sched.schedule_write(&top->rst, 1u); });
    sched.schedule_after(20, [&]() { sched.schedule_write(&top->rst, 0u); });

    // --- DRIVER (Source) ---
    // Reacts to Clock + Ready. Pushes data 0..15
    uint8_t data_to_send = 0;
    
    sched.add_sensitive_process({clk, rst}, [&](Scheduler& s) {
        if (clk->posedge() && !rst->val()) {
            // Default: Stop driving if we succeeded last cycle
            // (Simulating a "valid" pulse logic, though here we stream)
            
            // Check flow control
            if (s_tready->val()) {
                if (data_to_send < 16) {
                    // Drive new transaction
                    s.schedule_write(&top->s_tvalid, 1);
                    s.schedule_write(&top->s_tdata, data_to_send);
                    
                    std::cout << std::format("@{:4d} DRV: Sent 0x{:02X}\n", s.time(), data_to_send);
                    data_to_send++;
                } else {
                    // Done sending
                    s.schedule_write(&top->s_tvalid, 0);
                    s.schedule_write(&top->s_tdata, 0);
                }
            }
        }
    });

    // --- MONITOR (Sink) ---
    // Always Ready. Checks result on Valid.
    
    // Set m_tready = 1 permanently for this test (or toggle it to test backpressure!)
    sched.schedule_write(&top->m_tready, 1);

    sched.add_sensitive_process({clk}, [&](Scheduler& s) {
        if (clk->posedge() && m_tvalid->val() && m_tready->val()) {
            uint8_t actual = m_tdata->val();
            
            std::cout << std::format("@{:4d} MON: Got  0x{:02X} (Reversed)\n", s.time(), actual);
        }
    });

    // --- EVENT LISTENER (Async Reaction) ---
    sched.add_sensitive_process({event_out}, [&](Scheduler& s) {
        if (event_out->posedge()) {
            std::cout << std::format("\n!!! @{:4d} EVENT DETECTED: Counter reached 3 !!!\n\n", s.time());
        }
    });

    // 4. Run Simulation
    // Run for enough time to send 16 bytes
    std::cout << "Starting Simulation...\n";
    sched.run(contextp.get(), top.get(), tfp.get(), 500); 

    std::cout << "Simulation Finished.\n";
    
    // Final check
    if (data_to_send == 16) std::cout << "SUCCESS: All data sent.\n";
    else std::cout << "FAILURE: Timed out before sending data.\n";

    cout << format("Bye\n");
    
    // Cleanup
    top->final();
    tfp->close();
    contextp->statsPrintSummary();

    return 0;
}