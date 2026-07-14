/**
 * @brief C9: chrono-typed time on a 1 fs tick; durations await directly;
 *        run_until/run_for leave now == t; run() returns on stop() or
 *        quiescence.
 */

#include "t0.h"

#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

struct uni {
    scheduler s;
    std::vector<std::uint64_t> events;
};

// Events at 5, 10, 15, 25 ns; the 15 ns wake is a computed duration (parens
// needed - co_await binds tighter than +).
task<> emit(uni& u) {
    co_await 5_ns;          u.events.push_back(u.s.now().count());
    co_await 5_ns;          u.events.push_back(u.s.now().count());
    co_await (2_ns + 3_ns); u.events.push_back(u.s.now().count());
    co_await 10_ns;         u.events.push_back(u.s.now().count());
}

task<> stopper(scheduler& s, bool& ran_after) {
    co_await 5_ns;
    s.stop();
    co_await 100_ns;
    ran_after = true;   // must not run: stop() ended the loop at 5 ns
}

}

int main() {
    // Literal conversions on the femtosecond tick.
    CHECK(1_ps == sim_time{1000});
    CHECK(1_ns == 1000_ps);
    CHECK(1_us == 1000_ns);
    CHECK(5_ns == sim_time{5'000'000});
    CHECK((2_ns + 3_ns) == 5_ns);

    // run_until processes every slot in [now, t) and leaves now == t exactly.
    {
        uni u;
        u.s.spawn(emit(u), "emit");
        u.s.run_until(20_ns);

        CHECK(u.s.now() == 20_ns);
        CHECK(u.events.size() == 3);
        CHECK(u.events.back() == 15'000'000);

        u.s.run();   // drains the 25 ns event, then quiesces there
        CHECK(u.events.size() == 4);
        CHECK(u.s.now() == 25_ns);
    }

    // run_for(d) == run_until(now + d).
    {
        uni u;
        u.s.spawn(emit(u), "emit");
        u.s.run_for(12_ns);

        CHECK(u.s.now() == 12_ns);
        CHECK(u.events.size() == 2);
    }

    // stop() returns from run() promptly, in the requesting slot.
    {
        scheduler s;
        bool ran_after = false;
        s.spawn(stopper(s, ran_after), "stopper");
        s.run();

        CHECK(s.now() == 5_ns);
        CHECK(!ran_after);
    }

    println("  literals: 1 ps = 1000 fs, 1 ns = 1000 ps, 1 us = 1000 ns");
    println("  run_until(20 ns) processed 3 events and left now at 20 ns; run() quiesced at 25 ns");
    println("  run_for(12 ns) left now at 12 ns; stop() returned in the 5 ns slot");
    return t0_pass("t0_time");
}
