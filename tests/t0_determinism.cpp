/**
 * @brief C1: per-process observation sequences are invariant under spawn
 *        permutation. Every wait primitive participates: timed delays,
 *        edges, explicit sensitivity, rendezvous, and an until() poll.
 */

#include "t0.h"

#include <array>
#include <optional>
#include <utility>
#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

using trace = std::vector<std::pair<uint64_t, int>>;

struct universe {
    scheduler s;
    signal<bool> clk{s};
    signal<int> cnt{s, 0};
    signal<int> twice{s, 0};
    channel<int> ch{s};
    int drained = 0;
    std::array<trace, 5> log;

    void mark(int who, int what) {
        log[who].push_back({s.now().count(), what});
    }
};

task<> p_clock(universe& u) {
    for (int i = 0; i < 24; i++) {
        co_await delay(5_ns);
        u.clk <<= !u.clk;
    }
}

task<> p_count(universe& u) {
    for (;;) {
        co_await posedge(u.clk);
        u.cnt <<= u.cnt + 1;
        u.mark(0, u.cnt + 1);
    }
}

task<> p_comb(universe& u) {
    for (;;) {
        u.twice <<= u.cnt * 2;
        u.mark(1, u.cnt * 2);
        co_await changed(u.cnt);
    }
}

task<> p_send(universe& u) {
    for (int i = 0; i < 4; i++) {
        co_await delay(7_ns);
        bool ok = co_await u.ch.send(i * 10);
        u.mark(2, ok ? i * 10 : -1);
    }
    u.ch.close();
}

task<> p_recv(universe& u) {
    for (;;) {
        std::optional<int> v = co_await u.ch.recv();
        if (!v) {
            break;
        }
        u.mark(3, *v);
        u.drained++;
    }
}

task<> p_poll(universe& u) {
    co_await until([&] { return u.drained >= 3; });
    u.mark(4, u.drained);
}

struct run_result {
    std::array<trace, 5> log;
    int cnt;
    int twice;
};

run_result run_with(const std::array<int, 6>& order) {
    universe u;

    for (int idx : order) {
        switch (idx) {
            case 0: u.s.spawn(p_clock(u)); break;
            case 1: u.s.spawn(p_count(u)); break;
            case 2: u.s.spawn(p_comb(u)); break;
            case 3: u.s.spawn(p_send(u)); break;
            case 4: u.s.spawn(p_recv(u)); break;
            case 5: u.s.spawn(p_poll(u)); break;
        }
    }
    u.s.run();

    return run_result{
        .log   = u.log,
        .cnt   = u.cnt.get(),
        .twice = u.twice.get(),
    };
}

}

int main() {
    run_result a = run_with({0, 1, 2, 3, 4, 5});
    run_result b = run_with({5, 4, 3, 2, 1, 0});
    run_result c = run_with({3, 0, 5, 1, 4, 2});

    CHECK(a.log == b.log);
    CHECK(a.log == c.log);
    CHECK(a.cnt == b.cnt && a.cnt == c.cnt);
    CHECK(a.twice == b.twice && a.twice == c.twice);
    CHECK(a.cnt == 12);                    // 24 half-periods = 12 rises

    auto events = std::size_t{0};
    for (auto& t : a.log) {
        events += t.size();
    }
    println("  3 spawn orders x 6 processes: {} observations per run, traces identical, cnt={}",
            events, a.cnt);
    return t0_pass("t0_determinism");
}
