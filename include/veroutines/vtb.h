/**
 * @brief Transaction-level verification components over the veroutines kernel.
 *
 * The verification layer to the kernel's execution layer: where veroutines.h
 * gives coroutines, signals, edges and channels, this gives the pieces a
 * testbench is built from - an AXI4-Stream driver, sink and monitor, an
 * in-order scoreboard, and a pass/fail tracker whose verdict is the process
 * exit code. A component is a plain aggregate holding its ports, its channels
 * and a name, exposing one coroutine run(); the test spawns it and feeds it
 * transactions over a channel. Transactions are ordinary C++ values, so a
 * sequence is a channel you fill from a vector, a generator, or a model.
 */
#pragma once

#include <veroutines/veroutines.h>

#include <cstddef>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vtb {

using namespace veroutines;

// -----------------------------------------------------------------------------
// tracker
// -----------------------------------------------------------------------------

/**
 * @brief Records named pass/fail checks and renders a verdict.
 *
 * A check registered with expect() but never recorded counts as failed, so a
 * scoreboard that never ran fails the run. ok() holds only when every known
 * check has at least one pass and no fails; map it to the exit code.
 */
struct tracker {
    struct entry {
        std::size_t pass{0};
        std::size_t fail{0};
    };

    void expect(const std::string& name) { table.try_emplace(name); }

    bool record(const std::string& name, bool ok) {
        auto& e = table[name];
        if (ok) {
            ++e.pass;
        } else {
            ++e.fail;
            std::fprintf(stderr, "  FAIL %s (occurrence %zu)\n",
                         name.c_str(), e.pass + e.fail);
        }
        return ok;
    }

    bool ok() const {
        if (table.empty()) {
            return false;
        }
        for (auto& [name, e] : table) {
            if (e.fail != 0 || e.pass == 0) {
                return false;
            }
        }
        return true;
    }

    void report() const {
        std::printf("  -- checks --\n");
        for (auto& [name, e] : table) {
            std::printf("     %-18s pass=%zu fail=%zu\n", name.c_str(), e.pass, e.fail);
        }
    }

    std::map<std::string, entry> table;
};

// -----------------------------------------------------------------------------
// AXI4-Stream transaction
// -----------------------------------------------------------------------------

/**
 * @brief One AXI4-Stream beat. Data width is the template parameter; last and
 *        user default false so a data-only interface compares equal on payload.
 */
template<typename Data>
struct axis_beat {
    Data         data{};
    bool         last{false};
    bool         user{false};

    auto operator<=>(const axis_beat&) const = default;
};

namespace axis {

// ---- ports -------------------------------------------------------------------

/**
 * @brief A DUT slave port the testbench drives into: valid and payload are
 *        driven, ready is observed. last and user are optional (null when the
 *        interface omits them).
 */
template<typename Data>
struct in_port {
    model_in<bool>&  clk;
    model_in<bool>&  valid;
    model_out<bool>& ready;
    model_in<Data>&  data;
    model_in<bool>*  last{nullptr};
    model_in<bool>*  user{nullptr};
};

/**
 * @brief A DUT master port the testbench observes: valid and payload are
 *        observed, ready is driven by whoever consumes (a sink, or held high).
 */
template<typename Data>
struct out_port {
    model_in<bool>&  clk;
    model_out<bool>& valid;
    model_in<bool>&  ready;
    model_out<Data>& data;
    model_out<bool>* last{nullptr};
};

// ---- source ------------------------------------------------------------------

/**
 * @brief Drives beats from a channel onto a slave port, one handshake each.
 *
 * Pulls from seq until it closes, then leaves valid low. Each beat holds valid
 * until a rising edge sees ready, so it honors the slave's backpressure.
 */
template<typename Data>
struct source {
    in_port<Data>               port;
    channel<axis_beat<Data>>&   seq;
    std::string                 name{"axis_src"};

    task<> run() {
        for (;;) {
            auto beat = co_await seq.recv();
            if (!beat) {
                break;
            }
            port.data <<= beat->data;
            if (port.last) {
                *port.last <<= beat->last;
            }
            if (port.user) {
                *port.user <<= beat->user;
            }
            port.valid <<= true;
            do {
                co_await posedge(port.clk);
            } while (!port.ready);
            port.valid <<= false;
        }
        port.valid <<= false;
        co_return;
    }
};

// ---- sink --------------------------------------------------------------------

/**
 * @brief Consumes a master port and emits each accepted beat to a channel.
 *
 * Holds ready high; with stall_every set to N it drops ready for one cycle
 * after every N beats, exercising backpressure. Stops when the out channel
 * closes.
 */
template<typename Data>
struct sink {
    out_port<Data>              port;
    channel<axis_beat<Data>>&   out;
    unsigned                    stall_every{0};
    std::string                 name{"axis_sink"};

    task<> run() {
        port.ready <<= true;
        unsigned seen = 0;
        for (;;) {
            co_await posedge(port.clk);
            if (!(port.valid && port.ready)) {
                continue;
            }
            axis_beat<Data> beat{};
            beat.data = port.data.get();
            if (port.last) {
                beat.last = bool(port.last->get());
            }
            if (!co_await out.send(beat)) {
                break;
            }
            if (stall_every && (++seen % stall_every) == 0) {
                port.ready <<= false;
                co_await posedge(port.clk);
                port.ready <<= true;
            }
        }
        co_return;
    }
};

// ---- monitor -----------------------------------------------------------------

/**
 * @brief Passively observes a master port, emitting each accepted beat.
 *
 * Never drives ready - some other agent (or a held-high input) owns it. A beat
 * is accepted on a rising edge where valid and ready both hold.
 */
template<typename Data>
struct monitor {
    out_port<Data>              port;
    channel<axis_beat<Data>>&   out;
    std::string                 name{"axis_mon"};

    task<> run() {
        for (;;) {
            co_await posedge(port.clk);
            if (!(port.valid && port.ready)) {
                continue;
            }
            axis_beat<Data> beat{};
            beat.data = port.data.get();
            if (port.last) {
                beat.last = bool(port.last->get());
            }
            if (!co_await out.send(beat)) {
                break;
            }
        }
        co_return;
    }
};

}  // namespace axis

// -----------------------------------------------------------------------------
// scoreboard
// -----------------------------------------------------------------------------

/**
 * @brief Compares an actual channel against a golden sequence, in order.
 *
 * Records one match check per beat and, on drain, a count check that the
 * expected number arrived. The comparison lives here - the caller owns it by
 * choosing the transaction type and eq predicate (defaulting to equality).
 */
template<typename T, typename Eq = std::equal_to<>>
struct scoreboard {
    channel<T>&    actual;
    std::vector<T> golden;
    tracker&       trk;
    Eq             eq{};
    std::string    match_name{"beat.match"};
    std::string    count_name{"beat.count"};

    task<> run() {
        trk.expect(match_name);
        trk.expect(count_name);
        std::size_t i = 0;
        while (i < golden.size()) {
            auto a = co_await actual.recv();
            if (!a) {
                break;
            }
            trk.record(match_name, eq(golden[i], *a));
            ++i;
        }
        trk.record(count_name, i == golden.size());
        co_return;
    }
};

// -----------------------------------------------------------------------------
// collect
// -----------------------------------------------------------------------------

/**
 * @brief Drains a channel into a vector until it closes.
 *
 * A ready receiver for a monitor whose beats are inspected after the run, so
 * the monitor's unbuffered send never stalls.
 */
template<typename T>
task<> collect(channel<T>& ch, std::vector<T>& out) {
    for (;;) {
        auto v = co_await ch.recv();
        if (!v) {
            break;
        }
        out.push_back(std::move(*v));
    }
    co_return;
}

}  // namespace vtb
