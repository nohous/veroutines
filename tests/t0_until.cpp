/**
 * @brief C7: until(pred) polls once per delta round - the sound wait for
 *        plain C++ state; true at arm resumes without suspending; state
 *        mutated in a settled section is observed from the next slot.
 */

#include "t0.h"

#include <deque>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

struct uni {
    scheduler s;
    std::deque<int> q;       // plain state, no signal machinery
    int produced = 0;
    bool flag = false;
    sim_time consumer_woke{};
    sim_time flag_woke{};
    bool immediate_ok = false;
};

// Pushes onto a plain deque once per 5 ns: produced hits 3 at t = 15 ns.
task<> producer(uni& u) {
    for (int i = 0; i < 5; i++) {
        co_await 5_ns;
        u.q.push_back(i);
        u.produced++;
    }
}

// Wakes the round after the count first reaches 3, sees exactly three items.
task<> consumer(uni& u) {
    co_await until([&u] { return u.produced >= 3; });
    u.consumer_woke = u.s.now();
    CHECK(u.q.size() == 3);
}

// Predicate already true at arm: await_ready short-circuits, no time passes.
task<> immediate(uni& u) {
    co_await until([] { return true; });
    u.immediate_ok = (u.s.now() == 0_ns);
}

// Mutates plain state inside a settled section at slot 20; the poll loop is a
// delta-round phase, so the flag is not visible until a later slot.
task<> settle_writer(uni& u) {
    co_await 20_ns;
    co_await settled();
    u.flag = true;
}

task<> flag_waiter(uni& u) {
    co_await until([&u] { return u.flag; });
    u.flag_woke = u.s.now();
}

// A bare event at 25 ns so a slot exists after the settled write at 20 ns.
task<> heartbeat() {
    co_await 25_ns;
}

}

int main() {
    uni u;
    u.s.spawn(producer(u), "producer");
    u.s.spawn(consumer(u), "consumer");
    u.s.spawn(immediate(u), "immediate");
    u.s.spawn(settle_writer(u), "settle_writer");
    u.s.spawn(flag_waiter(u), "flag_waiter");
    u.s.spawn(heartbeat(), "heartbeat");
    u.s.run();

    CHECK(u.immediate_ok);
    CHECK(u.consumer_woke == 15_ns);
    CHECK(u.flag_woke == 25_ns);        // set in settled at 20 ns, seen the next slot

    println("  until(count>=3) woke at {} ns with 3 items queued", u.consumer_woke.count() / 1e6);
    println("  until(true) resumed at t=0 without suspending: {}", u.immediate_ok);
    println("  flag set in settled at 20 ns observed at {} ns (following slot)",
            u.flag_woke.count() / 1e6);
    return t0_pass("t0_until");
}
