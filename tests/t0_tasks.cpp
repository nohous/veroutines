/**
 * @brief C10: task composition - values return, exceptions propagate,
 *        awaited children complete before the parent resumes, and deep
 *        await chains hold constant stack.
 */

#include "t0.h"

#include <stdexcept>

using namespace veroutines;
using namespace veroutines::literals;

// ASan instrumentation blocks the symmetric-transfer tail call, so the
// sanitized build proves a short chain; the unsanitized build
// (t0_tasks_nosan in the Makefile) pins the full constant-stack depth.
#ifdef __SANITIZE_ADDRESS__
constexpr auto chain_len = 1000;
#else
constexpr auto chain_len = 100000;
#endif

namespace {

task<int> answer_after(sim_time d) {
    co_await d;   // durations await directly (C9)
    co_return 42;
}

task<int> boom() {
    throw std::runtime_error("boom");
    co_return 0;   // unreachable; keeps the coroutine shape
}

task<> noop() {
    co_return;
}

task<> parent(scheduler& s, int& got, bool& caught, bool& chained) {
    int v = co_await answer_after(3_ns);
    CHECK(s.now() == 3_ns);
    got = v;

    try {
        co_await boom();
    } catch (const std::runtime_error&) {
        caught = true;
    }

    for (auto i = 0; i < chain_len; i++) {
        co_await noop();
    }
    chained = true;
}

}

int main() {
    scheduler s;
    int got = 0;
    bool caught = false;
    bool chained = false;

    s.spawn(parent(s, got, caught, chained));
    s.run();

    CHECK(got == 42);
    CHECK(caught);
    CHECK(chained);

    println("  child returned {} after 3 ns; exception propagated and caught: {}", got, caught);
    println("  {} sequential awaits on flat stack (symmetric transfer)", chain_len);
    return t0_pass("t0_tasks");
}
