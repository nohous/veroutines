# veroutines - execution contract

veroutines is a discrete-event simulation kernel on C++20 coroutines:
VHDL's execution model, SV's edge vocabulary, C++'s types. Processes are
coroutines, signals carry staged (nonblocking) writes, and a verilated
model joins the loop as one more process. This file is normative; the
header carries orientation and the cycle, this file carries the clauses,
and each clause cites the tier-0 test that pins it.

## Vocabulary

- process       = a coroutine returning task<T>; runs until its next co_await
- signal<T>     = value with staged writes; sig <<= v stages, reads convert
- drive<T>      = staged write bound to a model input (functor or pointer)
- observed<T>   = model output sampled each round; carries edge state
- delta round   = one run/commit/eval/sample/wake pass within a time slot
- superstep     = all delta rounds of one time slot, plus the settled phase
- settled phase = VHDL postponed: runs once per slot, after convergence

## The cycle

    superstep at time T:
      round:   run runnable processes to suspension
               commit staged signals and drives     (the single update point)
               eval the model                       (internal convergence)
               sample observed values, derive edges
               wake satisfied waits
               -> repeat while any process ran or any signal changed (C8 bounds it)
      settled: postponed sections run once          (C6)
      advance: T = min(next timed wake, model next event);
               quiescent universe -> run() returns

## Clauses

C1 - determinism. A process's observation sequence - the times, values,
and wait outcomes it sees - is a function of the program, independent of
spawn order. Cross-process interleaving inside one run phase is
unspecified; everything observable through signals, waits, and channels
is invariant under it.

C2 - staged visibility. sig <<= v stages; the value becomes readable at
the round's commit point. Reads always return the last committed value.
The last stage in a round wins. operator= is deleted; <<= is the only
signal write.

C3 - single update point. Each delta round commits signals and drives
exactly once, before model eval; observed values refresh exactly once,
after eval. Edge state (prev/current/generation) derives only from these
two points.

C4 - edges and arming. Each committed change produces exactly one wake
window for edge waits: the round after the commit, in the same slot. A
wait observes only changes committed after it was armed (generation is
captured at arm time). rose()/fell() hold through the waking round.

C5 - wildcard convergence. changed(any) wakes when at least one signal
changed in the round. Re-staging an unchanged value is not a change -
this is the wildcard's termination guarantee: pure re-stagers reach a
fixpoint.

C6 - settled phase. co_await settled() resumes once per time slot, after
the delta loop converges; reads see the slot's final state. A settled
section must leave the slot quiescent: it suspends on a future event, and
staging a signal or completing a rendezvous from settled context is a
fatal error. Plain state mutated in settled context is observed by
until() from the next slot on.

C7 - until. until(pred) is polled: the predicate is evaluated once per
delta round (the lambda is opaque, so polling is the only sound
semantics), which makes it the correct wait for plain C++ state. The
waiter wakes in the round after the predicate first evaluates true.

C8 - bounded fixpoint. A slot whose delta rounds exceed the configured
bound (default 1000) is a fatal error, and the report names the processes
that ran and the signals that changed in the final round. spawn() accepts
an optional process name for exactly this report.

C9 - time. The canonical tick is 1 fs; the API is chrono-typed (sim_time,
std literals plus _ps/_fs). Durations await directly: co_await 25_ns is
delay(25_ns); a computed duration needs parens (co_await binds tighter
than arithmetic). Time advances to min(next timed wake, model next
event). run() returns on stop() or quiescence. run_until(t) processes
every slot in [now, t) and leaves now == t; run_for(d) is
run_until(now + d). Both return with the last processed slot fully
settled.

C10 - tasks. task<T> carries a value or an exception. co_await starts the
child immediately (symmetric transfer); the parent owns the frame and
resumes when the child completes, receiving the value or the rethrow.
Await chains run in constant stack; the tail call behind that needs an
optimized build (gcc -O2, clang -O1), and ASan blocks it, so the full
chain depth is pinned by the unsanitized test build. spawn() detaches:
the scheduler owns the frame. The scheduler reference rides the promise,
set at spawn and inherited parent to child, so awaitables take no
scheduler argument.

C11 - channels. send/recv rendezvous unbuffered; parked parties are
served in arrival order. close() resumes every parked party - recv yields
nullopt, send yields false - and subsequent operations complete
immediately the same way. Cancellation flows through these results.

C12 - frame destruction. The kernel can destroy any parked coroutine by
unlinking its promise from the wait structure that holds it, then
destroying the frame; locals unwind by RAII. This one mechanism serves
race() losers and scheduler teardown, which destroys all parked frames.
Every tier-0 test runs under ASan/UBSan, so a teardown leak fails the
suite.

## Clause-to-test map

| Clause | Test               | Status  |
|--------|--------------------|---------|
| C1     | t0_determinism.cpp | written |
| C2, C3 | t0_staging.cpp     | written |
| C4     | t0_edges.cpp       | written |
| C5     | t0_wildcard.cpp    | written |
| C6     | t0_settled.cpp     | written; fatal-from-settled is a fork death test |
| C7     | t0_until.cpp       | written |
| C8     | t0_livelock.cpp    | written; fork death test asserting the named report |
| C9     | t0_time.cpp        | written |
| C10    | t0_tasks.cpp       | written |
| C11    | t0_channel.cpp     | written |
| C12    | t0_race.cpp        | written; teardown also exercised by ASan in every test |

## Tier 1: cosim interlock (tests/t1/, verilated t1_top.sv)

Pins the boundary semantics of the cycle against real RTL, one binary,
four concurrent experiments:

- flop: a comb output lands in the driving slot; a flop takes a drive at
  the next edge; a drive co-staged with the clock edge is sampled by it,
  a drive staged after waking on that edge waits for the next one.
- echo: multi-cycle FSM handshake through drive/observed, exact fixed
  latency every round trip.
- svmaster: the SV side is the master (initial block, pure delays); the
  C++ side wakes at times supplied only by model_next_event.
- counters: coprime clock domains (10/7 ns), exact posedge counts, with
  both clocks toggling in one slot every 35 ns.
