#include <format>
#include <iostream>
#include "verilated.h"
#include "verilated_vcd_c.h"
#include "Vaxibox.h"
#include "veroutines.h"

using std::cout;
using std::format;

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

    TestbenchScheduler sched;

    top->clk = 0;
    top->rst = 0;
    top->s_tvalid = 0;
    top->s_tdata = 0;
    top->m_tready = 0;

    uint64_t clk_time = 0;
    std::function<void()> schedule_clk;

    schedule_clk = [&]() {
        top->clk = !top->clk;
        sched.schedule_after(5, schedule_clk);
    };
    schedule_clk(); 

    sched.always([&](const Signals& s) {
        if (s.posedge(&top->clk)) {
            cout << format("{:4d}: testbench: posedge clk\n", contextp->time());
        }
    }, &top->clk );

    sched.always([&](const Signals& s) {
        if (s.changed(&top->m_tvalid)) {
            cout << format("{:4d}: testbench: m_tvalid changed: {} -> {}\n", 
                    contextp->time(),
                    (int)s.prev_value(&top->m_tvalid),
                    (int)s.curr_value(&top->m_tvalid));
        }
    }, &top->m_tvalid);

    sched.always([&](const Signals& s) {
        if (s.changed(&top->event_out)) {
            cout << format("{:4d}: testbench: event_out changed: {} -> {}\n", 
                    contextp->time(),
                    (int)s.prev_value(&top->event_out),
                    (int)s.curr_value(&top->event_out));
        }
    }, &top->event_out);

    sched.schedule_at(25, [&]() {
        cout << format("{:4d}: testbench: setting s_tvalid=1, s_tdata=0x42\n", 
                     contextp->time());
        top->s_tvalid = 1;
        top->s_tdata = 0x42;
    });

    sched.schedule_at(35, [&]() {
        cout << format("{:4d}: testbench: setting m_tready=1\n", 
                     contextp->time());
        top->m_tready = 1;
    });

    // Main simulation loop
    tfp->dump(contextp->time());
    
    while (!contextp->gotFinish() && contextp->time() < 200) {
        // Find next event from either domain
        uint64_t next_cpp = sched.next_time_event();
        uint64_t next_model = top->eventsPending() ? top->nextTimeSlot() : UINT64_MAX;
        uint64_t next_time = std::min(next_cpp, next_model);
        
        if (next_time == UINT64_MAX) {
            cout << "No more events scheduled\n";
            break;
        }
        
        // Advance time
        contextp->time(next_time);
        
        // Execute C++ scheduled events (drives signals)
        sched.execute_time_events(next_time);

        bool do_cycle{true};
        for (unsigned i = 0; do_cycle ;i++) {
            cout << format("{:4d}: testbench: sched: delta {}\n", sched.current_time(), i);
            top->eval();
            sched.sample_signals();
            do_cycle = sched.check_signal_watchers();
        } 

        // Dump waveform
        tfp->dump(contextp->time());
    }

    // Cleanup
    top->final();
    tfp->close();
    contextp->statsPrintSummary();

    return 0;
}