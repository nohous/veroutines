/**
 * @brief C6: settled() resumes once per slot after the delta loop converges,
 *        reading the slot's final state; staging from a settled section is
 *        fatal.
 */

#include "t0_abort.h"

#include <tuple>
#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

// The port type is spelled veroutines::signal here: ::signal from <csignal> is
// in scope via t0_abort.h.
struct uni {
    scheduler s;
    veroutines::signal<int> a{s, 0};
    veroutines::signal<int> b{s, 0};
    veroutines::signal<int> c{s, 0};
    std::vector<std::tuple<std::uint64_t, int, int, int>> snaps;
};

// A two-hop combinational chain: a change on a drives b next round, which
// drives c the round after. The settled observer must see all three settled.
task<> relay_b(uni& u) { for (;;) { co_await changed(u.a); u.b <<= u.a + 1; } }
task<> relay_c(uni& u) { for (;;) { co_await changed(u.b); u.c <<= u.b + 1; } }

task<> driver(uni& u) {
    co_await 5_ns;   u.a <<= 10;
    co_await 5_ns;   u.a <<= 20;
}

task<> observer(uni& u) {
    for (;;) {
        co_await settled();
        u.snaps.push_back({u.s.now().count(), u.a.get(), u.b.get(), u.c.get()});
    }
}

// A settled section that stages a signal: the fatal path (C6). Run in a child.
task<> stage_in_settled(veroutines::signal<int>& x) {
    co_await settled();
    x <<= 1;
}

void run_stage_from_settled() {
    scheduler s;
    veroutines::signal<int> x{s, 0};
    s.spawn(stage_in_settled(x), "stager");
    s.run();
}

}

int main() {
    uni u;
    u.s.spawn(relay_b(u), "relay_b");
    u.s.spawn(relay_c(u), "relay_c");
    u.s.spawn(driver(u), "driver");
    u.s.spawn(observer(u), "observer");
    u.s.run();

    CHECK(u.snaps.size() == 3);
    CHECK(u.snaps[0] == std::make_tuple<std::uint64_t>(0, 0, 0, 0));
    CHECK(u.snaps[1] == std::make_tuple<std::uint64_t>(5'000'000, 10, 11, 12));
    CHECK(u.snaps[2] == std::make_tuple<std::uint64_t>(10'000'000, 20, 21, 22));

    // Staging a signal from a settled section is a fatal error.
    abort_result r = run_expecting_abort(run_stage_from_settled);
    CHECK(r.aborted);
    CHECK(r.err.find("settled") != std::string::npos);

    println("  settled once per slot at 0 / 5 / 10 ns, each seeing the converged (a,b,c)");
    println("  slot at 5 ns settled to (10,11,12): the two-hop comb chain fully relaxed");
    println("  staging from a settled section aborted as required");
    return t0_pass("t0_settled");
}
