# veroutines

A discrete-event simulation kernel built on C++ coroutines: **VHDL's execution
model, SystemVerilog's edge vocabulary, C++'s types.** Processes are coroutines,
signals carry staged (nonblocking) writes, and a verilated model joins the loop
as one more process - so you can write testbench logic, behavioral models, and
cosimulation glue as ordinary C++ that observes RTL timing semantics.

Header-only, C++23, no third-party build dependency.

```cpp
#include <veroutines/veroutines.h>
#include <cstdio>

using namespace veroutines;
using namespace veroutines::literals;

task<> clock_gen(signal<bool>& clk) {
    for (;;) {
        co_await 5_ns;
        clk <<= !clk;            // staged write: commits at the delta boundary
    }
}

task<> counter(signal<bool>& clk, int& hits) {
    for (;;) {
        co_await posedge(clk);   // wakes one delta round after the committed rise
        hits++;
    }
}

int main() {
    scheduler s;
    signal<bool> clk{s};
    int hits = 0;

    s.spawn(clock_gen(clk));
    s.spawn(counter(clk, hits));
    s.run_for(1_us);

    std::printf("%d rising edges\n", hits);   // 100
}
```

## The cycle

Time advances in superstep slots. Within a slot, delta rounds run until nothing
changes; then a settled (postponed) phase runs once; then time jumps to the next
event:

```
superstep at time T:
  round:   run runnable processes to suspension
           commit staged signals and drives     (the single update point)
           eval the model                        (internal convergence)
           sample observed values, derive edges
           wake satisfied waits
           -> repeat while any process ran or any signal changed (bounded)
  settled: postponed sections run once
  advance: T = min(next timed wake, model next event)
```

The staged write (`sig <<= v`) is the only signal write - it reads like the
nonblocking `<=` and commits at the round's single update point, so processes in
the same round see a consistent snapshot. `CONTRACT.md` is normative: twelve
clauses (C1-C12), each pinned by a tier-0 test.

## API at a glance

The everyday vocabulary reads like clocked RTL - a designer writing a
behavioral model reaches only for these:

- `signal<T>` - value with staged writes and edge derivation
- `model_in<T>` / `model_out<T>` - a model input port (driven) / output port (sampled after eval)
- `task<T>` - a process or subroutine; returns a value or propagates an exception, on a constant stack
- `co_await rising_edge(clk)` / `falling_edge` / `delay(d)` (or `co_await 25_ns`), and `sig <<= v`
- `process(s, rising_edge(clk), body)` / `comb(s, body)` - VHDL-flavored clocked and combinational processes

The rest is the observer / verification tier - a monitor or a cosim comb bridge
reaches for these, everyday modeling code does not:

- `changed(sigs...)` (explicit sensitivity), `changed(any)` (wildcard), `until(pred)` (poll plain state)
- `settled()` - the postponed phase: the slot's converged state, once per slot. Ordinary clocked reads never need it, because the register boundary already hides deltas; it earns its keep observing within-slot combinational settling (see the comb-partition example)
- `channel<T>` - unbuffered rendezvous with `close()`
- `race(aws...)` - run awaitables concurrently, resume with the winner's index, cancel the rest
- `wave` - a settled-phase VCD tracer: C++ signals and model signals (via `model_out`) in one file
- `scheduler` - `spawn`, `run` / `run_until` / `run_for`, `stop`, and the `eval_model` / `model_next_event` hooks a verilated model binds through

## Writing a design

The HDL surface is VHDL-flavored sugar over the primitives, so it reads like an
architecture body - `process` for a clocked (edge-triggered) process, `comb` for
a concurrent combinational statement, `rising_edge`/`falling_edge` as triggers:

```cpp
struct counter {                              // the architecture body
    signal<bool>&    clk;
    signal<uint8_t>  q;
    signal<uint8_t>  q2;

    counter(scheduler& s, signal<bool>& clk_) : clk(clk_), q{s}, q2{s} {
        process(s, rising_edge(clk), [&]{ q <<= q + 1; });   // clocked
        comb(s, [&]{ q2 <<= q * 2; });                       // combinational
    }
};
```

`comb` re-evaluates on any change and converges by the fixpoint, so it carries
no sensitivity list. A combinational output settles one delta round after its
input, so read it at `settled()`.

## Waveform tracing

`wave` writes a single VCD live from the settled phase - C++ signals and any
model signal you surface through `model_out`, in one file, with no post-merge:

```cpp
wave w(s, "run.vcd", "tb");
w.scope("cpp");  w.add("addr", addr);   w.add("mask", mask);     w.pop();
w.scope("rtl");  w.add("rdata", rdata); w.add("result", result); w.pop();
// ... run; the file carries both heads on one timeline
```

It samples at `settled()`, so the trace shows one value per slot - the slot's
converged state, not delta transients. Register signals before `run()`. For a
model's internal signals, expose them (verilator `--public`) and bind a
`model_out` to trace them alongside the C++ side.

## Building

CMake-first. As a consumer:

```cmake
find_package(veroutines REQUIRED)         # or FetchContent
target_link_libraries(your_target PRIVATE veroutines::veroutines)
```

To build and run the test suite and demo from a checkout:

```sh
cmake --preset dev
cmake --build build
ctest --test-dir build --output-on-failure
```

Every tier-0 test runs under AddressSanitizer and UBSan, so a leaked or
use-after-free coroutine frame fails the suite.

## Tests

- **Tier 0** (`tests/`) - standalone DES, one `main` per clause: staged
  visibility, edges seen once, spawn-order determinism, rendezvous, wildcard
  convergence, settled ordering, the bounded-fixpoint death test, and `race`
  cancellation.
- **Tier 1** (`tests/t1/`) - cosim interlock against a verilated behavioral
  module (`--timing`): flop/comb boundary timing, an FSM handshake, an
  SV-master direction, and coprime clock domains. Built only when verilator is
  discoverable.

## Examples

Illustrative cosim programs pairing a verilated behavioral model with a C++
behavioral model (`examples/`, built when verilator is found, self-checking):

- **`uart_link`** - a behavioral RTL UART transmitter and a C++ oversampling
  receiver, one wire (`txd`) across the boundary. The everyday vocabulary only:
  edges, clocked sampling, staged writes. The "feels at home" starting point.
- **`comb_partition`** - a combinational datapath cut by the boundary: the comb
  path crosses twice (`addr -> rdata -> mask -> result`), acyclic, no clock, so
  the delta loop relaxes it to a fixpoint within one slot. Shows the dual-head
  convergence directly - a traced vector prints the intra-slot relaxation from a
  transient (stale crossing) to the settled value.

## Demo

`demo/ring.cpp` is a micro token-ring readout - a pure-DES playground that puts
every wait primitive on stage. Run `ring 100 40 8` and watch the queues climb.
