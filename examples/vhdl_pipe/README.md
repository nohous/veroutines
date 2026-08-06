# vhdl_pipe

Bring a VHDL DUT into veroutines: `ghdl synth -> Verilog -> verilator -> a
veroutines C++ testbench`, with a full-hierarchy FST out. The VHDL sibling of
the `uart_link` / `merged_trace` cosim examples.

One CMake build, no yosys: `cmake/GhdlSynth.cmake` synthesizes each DUT with
ghdl's own backend and feeds the netlist to `verilate()`; the testbench links
veroutines and becomes a `ctest`.

## Run

```sh
./run.sh              # configure, build, and run all tests
./run.sh -R queues    # one test (extra args pass to ctest)
./run.sh -V           # verbose (see scenario output)
```

`run.sh` loads the oss-cad-suite tools (ghdl, verilator), then runs
`cmake -S . -B build`, `cmake --build build`, `ctest`. Synth runs at configure
time (verilator enumerates its outputs then); the VHDL sources are configure
dependencies, so editing one and re-building re-synthesizes automatically.

View a wave (each test traces to its build dir):

```sh
surfer build/duts/queues/dut.fst
```

## Adding a DUT

A DUT is a folder under `duts/<name>/` with a `CMakeLists.txt` that calls one
helper, plus the testbench:

```cmake
add_vhdl_verilated_test(<name>
    TOP   <entity>
    VHDL  <sources in ghdl analysis order - packages first>
    TB    <name>_tb.cpp
    [GENERICS NAME=VALUE ...])   # top-level generic overrides -> ghdl -g
```

The testbench does `#include "Vdut.h"`, drives the verilated top through
`model_in` / `model_out`, and returns nonzero to fail the test. Register the DUT
in the top `CMakeLists.txt` with `add_subdirectory(duts/<name>)`.

The shipped `probe` DUT is a self-contained self-test (register, comb net,
record, sub-hierarchy). DUTs that reference RTL outside this repo are gitignored
and added only when present.

## cmake/GhdlSynth.cmake

`ghdl_synth_verilog(<out> TOP <e> SOURCES <vhd...> [STD] [FLAGS] [GENERICS])`
synthesizes VHDL to a Verilog netlist via ghdl (no yosys) and returns its path;
`add_vhdl_verilated_test()` wraps synth -> verilate (PREFIX Vdut, FST) -> link
veroutines -> `add_test`. Reusable in any veroutines-based project.
