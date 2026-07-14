/**
 * @brief C5: changed(any) wakes on any committed change; re-staging an
 *        unchanged value is not a change, so pure re-stagers converge.
 */

#include "t0.h"

#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

struct uni {
    scheduler s;
    signal<int> a{s, 0};
    signal<int> b{s, 0};
    signal<int> r{s, 3};
    int wakes = 0;
    std::vector<std::uint64_t> woke_at;
};

// Wildcard observer: wakes the round after any signal commits a change.
task<> observer(uni& u) {
    co_await changed(any);   // arm past the t=0 no-op commit
    for (;;) {
        u.wakes++;
        u.woke_at.push_back(u.s.now().count());
        co_await changed(any);
    }
}

// Pure re-stager on its own signal: stages the committed value back every
// wake. If re-staging counted as a change this would livelock; it converges,
// so the mere completion of run() is the termination guarantee (C5).
task<> restager(uni& u) {
    for (;;) {
        u.r <<= u.r;
        co_await changed(any);
    }
}

// Real change at 5 (a) and 10 (b), a no-op re-stage at 15 (a <<= its current
// value, not a change), then a real change at 20 (a).
task<> driver(uni& u) {
    co_await 5_ns;   u.a <<= 1;
    co_await 5_ns;   u.b <<= 2;
    co_await 5_ns;   u.a <<= 1;
    co_await 5_ns;   u.a <<= 7;
}

}

int main() {
    uni u;
    u.s.spawn(observer(u), "observer");
    u.s.spawn(restager(u), "restager");
    u.s.spawn(driver(u), "driver");
    u.s.run();

    CHECK(u.wakes == 3);
    CHECK(u.woke_at.size() == 3);
    CHECK(u.woke_at[0] == 5'000'000);    // a changed
    CHECK(u.woke_at[1] == 10'000'000);   // b changed
    CHECK(u.woke_at[2] == 20'000'000);   // a changed; the 15 ns re-stage stayed silent
    CHECK(u.r.get() == 3);               // re-stager never moved its signal

    println("  wildcard woke at 5 / 10 / 20 ns, silent on the 15 ns no-op re-stage");
    println("  pure re-stager converged: run() terminated, r held at {}", u.r.get());
    return t0_pass("t0_wildcard");
}
