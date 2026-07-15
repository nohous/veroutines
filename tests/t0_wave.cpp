/**
 * @brief Waveform tracing: wave writes a VCD from the settled phase - one file,
 *        header plus change-only value dumps, sampled once per slot.
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
        signal<bool> clk{s};
        signal<std::uint8_t> count{s, 0};

        wave w(s, path, "tb");
        w.add("clk", clk);        // code ! (first), 1 bit
        w.add("count", count);    // code " , 8 bits

        process(s, rising_edge(clk), [&] { count <<= count + 1; });
        s.spawn(gen_clock(clk, 6), "clk");   // rises at 5, 15, 25 ns -> count 3
        s.run();
    }   // wave destructs here, flushing and closing the file

    std::string vcd = slurp(path);

    CHECK(vcd.find("$timescale 1fs $end") != std::string::npos);
    CHECK(vcd.find("$scope module tb $end") != std::string::npos);
    CHECK(vcd.find("$var wire 1 ! clk $end") != std::string::npos);
    CHECK(vcd.find("$var wire 8 \" count $end") != std::string::npos);
    CHECK(vcd.find("$dumpvars") != std::string::npos);
    CHECK(vcd.find("b00000011 \"") != std::string::npos);   // count reached 3
    CHECK(vcd.find("#25000000") != std::string::npos);      // last rise at 25 ns

    std::remove(path);

    println("  wrote a well-formed VCD: timescale, scoped $var decls, $dumpvars,");
    println("  and change-only dumps; count reached 3 at 25 ns");
    return t0_pass("t0_wave");
}
