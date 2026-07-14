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

- `signal<T>` - value with staged writes and edge derivation
- `drive<T>` / `observed<T>` - a model input (staged, sinks on commit) / output (sampled after eval)
- Waits: `delay(d)` (or `co_await 25_ns`), `posedge`/`negedge`, `changed(sigs...)`, `changed(any)`, `until(pred)`, `settled()`
- `task<T>` - a process or subroutine; returns a value or propagates an exception, on a constant stack
- `channel<T>` - unbuffered rendezvous with `close()`
- `race(aws...)` - run awaitables concurrently, resume with the winner's index, cancel the rest
- `scheduler` - `spawn`, `run` / `run_until` / `run_for`, `stop`, and the `eval_model` / `model_next_event` hooks a verilated model binds through

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

## Demo

`demo/ring.cpp` is a micro token-ring readout - a pure-DES playground that puts
every wait primitive on stage. Run `ring 100 40 8` and watch the queues climb.
