/**
 * @brief C11: unbuffered rendezvous both arrival orders, parked senders
 *        served in arrival order, close() resumes every parked party.
 */

#include "t0.h"

#include <optional>
#include <vector>

using namespace veroutines;
using namespace veroutines::literals;

namespace {

task<> send_one(channel<int>& ch, int v, bool& ok) {
    ok = co_await ch.send(v);
}

task<> send_after(channel<int>& ch, sim_time d, int v, bool& ok) {
    co_await delay(d);
    ok = co_await ch.send(v);
}

task<> recv_n(channel<int>& ch, int n, std::vector<int>& got) {
    for (int i = 0; i < n; i++) {
        std::optional<int> v = co_await ch.recv();
        CHECK(v.has_value());
        got.push_back(*v);
    }
}

task<> recv_after(channel<int>& ch, sim_time d, std::optional<int>& got,
                  bool& done) {
    co_await delay(d);
    got = co_await ch.recv();
    done = true;
}

task<> recv_expect_closed(channel<int>& ch, bool& done) {
    std::optional<int> v = co_await ch.recv();
    CHECK(!v.has_value());
    done = true;
}

task<> close_after(channel<int>& ch, sim_time d) {
    co_await delay(d);
    ch.close();
}

}

int main() {
    // Sender parks first; the value crosses when the receiver arrives.
    {
        scheduler s;
        channel<int> ch{s};
        bool ok = false;
        std::optional<int> got;
        bool done = false;

        s.spawn(send_one(ch, 7, ok));
        s.spawn(recv_after(ch, 5_ns, got, done));
        s.run();

        CHECK(ok);
        CHECK(done);
        CHECK(got == 7);
    }

    // Receiver parks first; the sender's arrival completes the rendezvous.
    {
        scheduler s;
        channel<int> ch{s};
        std::vector<int> got;
        bool ok = false;

        s.spawn(recv_n(ch, 1, got));
        s.spawn(send_after(ch, 5_ns, 9, ok));
        s.run();

        CHECK(ok);
        CHECK(got.size() == 1);
        CHECK(got[0] == 9);
    }

    // Three parked senders drain in arrival order.
    {
        scheduler s;
        channel<int> ch{s};
        bool ok1 = false, ok2 = false, ok3 = false;
        std::vector<int> got;

        s.spawn(send_one(ch, 1, ok1));
        s.spawn(send_one(ch, 2, ok2));
        s.spawn(send_one(ch, 3, ok3));
        s.spawn(recv_n(ch, 3, got));
        s.run();

        CHECK(ok1 && ok2 && ok3);
        CHECK(got.size() == 3);
        CHECK(got[0] == 1);
        CHECK(got[1] == 2);
        CHECK(got[2] == 3);
    }

    // close() resumes a parked receiver with nullopt.
    {
        scheduler s;
        channel<int> ch{s};
        bool done = false;

        s.spawn(recv_expect_closed(ch, done));
        s.spawn(close_after(ch, 5_ns));
        s.run();

        CHECK(done);
    }

    // close() resumes a parked sender with false.
    {
        scheduler s;
        channel<int> ch{s};
        bool ok = true;

        s.spawn(send_one(ch, 5, ok));
        s.spawn(close_after(ch, 5_ns));
        s.run();

        CHECK(!ok);
    }

    // Operations on a closed channel complete immediately the same way.
    {
        scheduler s;
        channel<int> ch{s};
        bool ok = true;
        bool done = false;

        ch.close();
        s.spawn(send_one(ch, 5, ok));
        s.spawn(recv_expect_closed(ch, done));
        s.run();

        CHECK(!ok);
        CHECK(done);
    }

    println("  rendezvous both arrival orders; 3 parked senders drained FIFO;");
    println("  close(): parked recv -> nullopt, parked send -> false, post-close ops immediate");
    return t0_pass("t0_channel");
}
