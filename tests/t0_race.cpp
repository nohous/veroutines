/**
 * @brief C12: race() runs awaitables concurrently, resumes with the winner's
 *        index, and destroys every loser - including the await chains they
 *        own. Under ASan/UBSan a missed unlink or leaked frame fails here.
 */

#include "t0.h"

#include <optional>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

struct probe {
    std::optional<std::size_t> idx{};
    sim_time when{};
};

task<> sleep_for(sim_time d, bool& done) {
    co_await d;
    done = true;
}

task<> recv_forever(channel<int>& ch, bool& done) {
    co_await ch.recv();   // no sender ever arrives
    done = true;
}

// A three-deep await chain whose leaf parks far in the future; racing it
// against a short timeout must destroy all three frames and unlink the leaf.
task<> chain_leaf() { co_await 1000_ns; }
task<> chain_mid() { co_await chain_leaf(); }
task<> chain_top(bool& done) { co_await chain_mid(); done = true; }

task<> race_timeout(scheduler& s, probe& p, bool& work_done) {
    p.idx = co_await race(sleep_for(50_ns, work_done), delay(30_ns));
    p.when = s.now();
}

task<> race_work_wins(scheduler& s, probe& p, bool& work_done) {
    p.idx = co_await race(sleep_for(20_ns, work_done), delay(30_ns));
    p.when = s.now();
}

task<> race_channel(scheduler& s, channel<int>& ch, probe& p, bool& recv_done) {
    p.idx = co_await race(recv_forever(ch, recv_done), delay(30_ns));
    p.when = s.now();
}

task<> race_three(scheduler& s, probe& p) {
    p.idx = co_await race(delay(30_ns), delay(10_ns), delay(20_ns));
    p.when = s.now();
}

task<> race_deep(scheduler& s, probe& p, bool& deep_done) {
    p.idx = co_await race(chain_top(deep_done), delay(10_ns));
    p.when = s.now();
}

}

int main() {
    // The timeout wins: the work loses and its frame is cancelled mid-delay.
    {
        scheduler s;
        probe p;
        bool work_done = false;
        s.spawn(race_timeout(s, p, work_done), "driver");
        s.run();

        CHECK(p.idx == 1);
        CHECK(p.when == 30_ns);
        CHECK(!work_done);
        CHECK(s.now() == 30_ns);   // the cancelled 50 ns delay did not pollute time
    }

    // The work wins: the timeout is the loser this time.
    {
        scheduler s;
        probe p;
        bool work_done = false;
        s.spawn(race_work_wins(s, p, work_done), "driver");
        s.run();

        CHECK(p.idx == 0);
        CHECK(p.when == 20_ns);
        CHECK(work_done);
    }

    // The loser is parked in a channel rendezvous: cancel must unlink it.
    {
        scheduler s;
        channel<int> ch{s};
        probe p;
        bool recv_done = false;
        s.spawn(race_channel(s, ch, p, recv_done), "driver");
        s.run();

        CHECK(p.idx == 1);
        CHECK(p.when == 30_ns);
        CHECK(!recv_done);
    }

    // Three racers: the middle argument wins, the other two are cancelled.
    {
        scheduler s;
        probe p;
        s.spawn(race_three(s, p), "driver");
        s.run();

        CHECK(p.idx == 1);
        CHECK(p.when == 10_ns);
        CHECK(s.now() == 10_ns);   // cancelled 20 / 30 ns delays purged, not visited
    }

    // A deep await chain loses: all three frames unwind, the leaf unlinks.
    {
        scheduler s;
        probe p;
        bool deep_done = false;
        s.spawn(race_deep(s, p, deep_done), "driver");
        s.run();

        CHECK(p.idx == 1);
        CHECK(p.when == 10_ns);
        CHECK(!deep_done);
    }

    // Teardown: a frame still parked when the scheduler dies must be destroyed
    // (C12). ASan flags a leak if the parked root survives.
    {
        scheduler s;
        bool never = false;
        s.spawn(sleep_for(1_us, never), "sleeper");
        s.run_for(10_ns);
        CHECK(!never);
    }

    println("  timeout/work winners resolve at 30 / 20 ns; the loser frame is cancelled");
    println("  losers parked in a channel, three-deep, or a chain all unlink cleanly");
    println("  cancelled far-future delays never advance time; teardown drops parked frames");
    return t0_pass("t0_race");
}
