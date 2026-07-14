/**
 * @brief Micro token-ring readout: kernel demo, pure DES, no verilator.
 *
 * Eight stations on a token ring, Poisson packet injection, one packet
 * per clock while a station holds the token, token passes on empty - a
 * pocket version of the tpx3 EoC drain (with a 1-hop-per-cycle token,
 * where the real chip has a combinational bypass). Every wait primitive
 * is on stage: posedge (stations), negedge + settled (the monitor),
 * until on plain state (the orchestrator), rendezvous channel (the bus),
 * timed delays (clock and injectors).
 *
 * ./ring [mean_gap_ns] [sim_us] [hot_mult] [print_every]
 *   mean_gap_ns  mean packet gap per station     (default 800)
 *   sim_us       injection window                (default 20)
 *   hot_mult     rate multiplier for station 0   (default 1)
 *   print_every  monitor cadence, clock cycles   (default 25)
 *
 * Things to try: crank the rate (./ring 100) and watch the queue bars
 * climb while the token camps on backlogged stations; make station 0
 * hot (./ring 400 40 8) and watch it hog the ring.
 */

#include <veroutines/veroutines.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <version>

#if defined(__cpp_lib_print)
#include <print>
using std::println;
#else
#include <format>
template<typename... A>
void println(std::format_string<A...> f, A&&... a) {
    auto s = std::format(f, std::forward<A>(a)...);
    s.push_back('\n');
    std::fputs(s.c_str(), stdout);
}
#endif

using namespace veroutines;
using namespace veroutines::literals;

constexpr auto n_stations = 8;
constexpr auto clk_period = 10_ns;   // 100 MHz

struct packet {
    int station;
    int seq;
    std::uint64_t t_gen;
};

struct ring_sim {
    scheduler s;
    signal<bool> clk{s};
    signal<int> token{s, 0};
    channel<packet> bus{s};

    std::array<std::deque<packet>, n_stations> q;
    std::array<int, n_stations> per_station{};

    std::uint64_t generated  = 0, drained = 0;
    double        sum_lat_ns = 0.0;
    std::uint64_t max_lat_fs = 0;
    bool          stim_off   = false;
    std::mt19937 rng{1};
};

static auto total_queued(const ring_sim& d) {
    auto n = std::size_t{0};

    for (auto& q : d.q) {
        n += q.size();
    }

    return n;
}

static char depth_glyph(std::size_t n) {
    constexpr char glyphs[] = " .:-=+*#%@";
    return glyphs[std::min(n, std::size_t{9})];
}

static task<> clock_gen(ring_sim& d) {
    for (;;) {
        co_await (clk_period / 2);   // durations await directly; parens: co_await binds tighter than /
        d.clk <<= !d.clk;
    }
}

static task<> injector(ring_sim& d, int me, double mean_gap_ns) {
    std::exponential_distribution<double> gap(1.0 / mean_gap_ns);
    auto seq = 0;

    for (;;) {
        co_await sim_time{std::uint64_t(gap(d.rng) * 1e6) + 1};
        if (d.stim_off) {
            co_return;
        }

        d.q[me].push_back({me, seq++, d.s.now().count()});
        d.generated++;
    }
}

// One packet per clock while holding the token; pass on empty. The
// token signal is staged, so a pass lands next delta and the ring walks
// one hop per cycle when idle.
static task<> station(ring_sim& d, int me) {
    for (;;) {
        co_await posedge(d.clk);
        if (d.token.get() != me) {
            continue;
        }

        if (!d.q[me].empty()) {
            auto p = d.q[me].front();
            d.q[me].pop_front();
            co_await d.bus.send(p);
        } else {
            d.token <<= (me + 1) % n_stations;
        }
    }
}

static task<> sink(ring_sim& d) {
    for (;;) {
        auto p = co_await d.bus.recv();
        if (!p) {
            break;
        }

        auto lat = d.s.now().count() - p->t_gen;
        d.drained++;
        d.per_station[p->station]++;
        d.sum_lat_ns += double(lat) / 1e6;
        d.max_lat_fs = std::max(d.max_lat_fs, lat);
    }
}

// The settled monitor: negedge picks the slot, settled() defers past its
// delta loop, so every line is a converged snapshot of the cycle.
static task<> monitor(ring_sim& d, int every) {
    auto cycle = 0;

    for (;;) {
        co_await negedge(d.clk);
        co_await settled();

        if (cycle++ % every != 0) {
            continue;
        }

        auto bars = std::string{};
        for (auto& q : d.q) {
            bars.push_back(depth_glyph(q.size()));
        }
        println("{:>8.2f} us  tok={} [{}]  queued={:<4} gen={:<5} drained={}",
                double(d.s.now().count()) / 1e9, d.token.get(), bars,
                total_queued(d), d.generated, d.drained);
    }
}

static task<> orchestrator(ring_sim& d, sim_time window) {
    co_await window;
    d.stim_off = true;
    println("-- injection window over at {:.2f} us, draining --",
            double(d.s.now().count()) / 1e9);

    co_await until([&d] { return total_queued(d) == 0; });
    co_await 1_us;

    d.bus.close();
    d.s.stop();
}

int main(int argc, char** argv) {
    auto mean_gap_ns = argc > 1 ? std::atof(argv[1]) : 800.0;
    auto sim_us      = argc > 2 ? std::atof(argv[2]) : 20.0;
    auto hot_mult    = argc > 3 ? std::atof(argv[3]) : 1.0;
    auto every       = argc > 4 ? std::atoi(argv[4]) : 25;

    ring_sim d;
    println("ring: {} stations at {:.0f} MHz, mean gap {:.0f} ns/station"
            " (station 0 x{:.0f}), window {:.0f} us",
            n_stations, 1e9 / double(clk_period.count()), mean_gap_ns,
            hot_mult, sim_us);
    println("queue bars, one glyph per station, ' ' empty to '@' 9+ deep:");
    println("");

    d.s.spawn(clock_gen(d), "clk");
    for (auto i = 0; i < n_stations; i++) {
        auto gap = i == 0 ? mean_gap_ns / hot_mult : mean_gap_ns;
        d.s.spawn(injector(d, i, gap), std::format("inj{}", i));
        d.s.spawn(station(d, i), std::format("station{}", i));
    }
    d.s.spawn(sink(d), "sink");
    d.s.spawn(monitor(d, every), "monitor");
    d.s.spawn(orchestrator(d, sim_time{std::uint64_t(sim_us * 1e9)}), "orchestrator");

    d.s.run();

    auto conserved = d.generated == d.drained;
    println("");
    println("summary: generated {} drained {} conserved {}",
            d.generated, d.drained, conserved ? "yes" : "NO");
    if (d.drained > 0) {
        println("latency: mean {:.1f} ns, max {:.1f} ns",
                d.sum_lat_ns / double(d.drained), double(d.max_lat_fs) / 1e6);
    }

    println("");
    println("station  drained  share");
    for (auto i = 0; i < n_stations; i++) {
        auto width = d.drained ? d.per_station[i] * 40 / int(d.drained) : 0;
        println("{:>7}  {:>7}  {}", i, d.per_station[i], std::string(width, '#'));
    }

    return conserved ? 0 : 1;
}
