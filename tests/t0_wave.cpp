/**
 * @brief Waveform tracing: named signals self-register and wave::add_all()
 *        sweeps them; a dotted name nests into VCD scopes. The tracer writes
 *        a VCD from the settled phase - header plus change-only value dumps.
 */

#include "t0.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

task<> gen_clock(signal<bool>& clk, int halves) {
    for (int i = 0; i < halves; i++) {
        co_await 5_ns;
        clk <<= !clk;
    }
}

std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}

int main() {
    const char* path = "t0_wave.vcd";

    {
        scheduler s;
        signal<bool> clk{s, "clk"};                    // named -> self-registers
        signal<std::uint8_t> count{s, "dut.count"};    // dotted name -> nested scope

        wave w(s, path, "tb");
        w.add_all();                                   // sweeps every named signal

        process(s, rising_edge(clk), [&] { count <<= count + 1; });
        s.spawn(gen_clock(clk, 6), "clk");             // rises at 5, 15, 25 ns -> count 3
        s.run();
    }   // wave destructs here, flushing and closing the file

    std::string vcd = slurp(path);

    CHECK(vcd.find("$timescale 1fs $end") != std::string::npos);
    CHECK(vcd.find("$scope module tb $end") != std::string::npos);
    CHECK(vcd.find("$var wire 1 ! clk $end") != std::string::npos);   // clk at top
    CHECK(vcd.find("$scope module dut $end") != std::string::npos);   // dotted -> nested
    CHECK(vcd.find("count $end") != std::string::npos);
    CHECK(vcd.find("b00000011") != std::string::npos);                // count reached 3
    CHECK(vcd.find("#25000000") != std::string::npos);                // last rise at 25 ns

    std::remove(path);

    println("  named signals self-registered; add_all() swept them, nesting dut.count");
    println("  into a dut scope; count reached 3 at 25 ns");
    return t0_pass("t0_wave");
}
