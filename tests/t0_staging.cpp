/**
 * @brief C2/C3: staged writes commit once per round; reads see committed
 *        state; the last stage in a round wins.
 */

#include "t0.h"

#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

// The temp-free swap: both processes read the other signal's committed
// value in the same round, so the swap needs no intermediate.
task<> swap_a(signal<int>& x, signal<int>& y) {
    x <<= y;
    co_return;
}

task<> swap_b(signal<int>& x, signal<int>& y) {
    y <<= x;
    co_return;
}

task<> last_wins(signal<int>& z) {
    z <<= 5;
    z <<= 7;
    co_return;
}

task<> read_back(signal<int>& a, std::vector<int>& seen) {
    a <<= 41;
    seen.push_back(a);        // pre-commit: still the initial value

    co_await changed(a);
    seen.push_back(a);        // post-commit
}

}

int main() {
    scheduler s;
    signal<int> x{s, 1};
    signal<int> y{s, 2};
    signal<int> z{s, 0};
    signal<int> a{s, 0};
    std::vector<int> seen;

    s.spawn(swap_a(x, y));
    s.spawn(swap_b(x, y));
    s.spawn(last_wins(z));
    s.spawn(read_back(a, seen));
    s.run();

    CHECK(x.get() == 2);
    CHECK(y.get() == 1);
    CHECK(z.get() == 7);
    CHECK(seen.size() == 2);
    CHECK(seen[0] == 0);
    CHECK(seen[1] == 41);

    println("  swap without a temp: x,y = 1,2 -> {},{}", x.get(), y.get());
    println("  read-back: staged 41, saw {} pre-commit, {} post-commit", seen[0], seen[1]);
    return t0_pass("t0_staging");
}
