/**
 * @brief C8: a time slot whose delta rounds exceed the bound is fatal, and
 *        the report names the process that ran in the final round.
 */

#include "t0_abort.h"

using namespace veroutines;
using namespace veroutines::literals;

namespace {

// Commits a real change every round it wakes, so the wildcard never reaches a
// fixpoint: the slot at t=0 spins until the round bound trips (C8). The port
// type is spelled veroutines::signal - ::signal from <csignal> is in scope
// here (see t0_abort.h).
task<> spinner(veroutines::signal<int>& x) {
    for (;;) {
        x <<= x.get() + 1;
        co_await changed(any);
    }
}

void run_livelock() {
    scheduler s;
    veroutines::signal<int> x{s, 0};
    s.spawn(spinner(x), "spinner");
    s.run();
}

}

int main() {
    abort_result r = run_expecting_abort(run_livelock);

    CHECK(r.aborted);
    CHECK(r.err.find("spinner") != std::string::npos);   // named the churning process
    CHECK(r.err.find("1000") != std::string::npos);      // the configured round bound
    CHECK(r.err.find("delta rounds") != std::string::npos);

    println("  a non-converging slot aborted after the round bound");
    println("  the report named 'spinner' and the 1000-round bound");
    return t0_pass("t0_livelock");
}
