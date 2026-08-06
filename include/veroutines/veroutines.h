/**
 * @brief veroutines - discrete-event simulation kernel on C++ coroutines.
 *
 * VHDL's execution model, SV's edge vocabulary, C++'s types: processes are
 * coroutines returning task<T>, signals carry staged writes (sig <<= v),
 * and a verilated model joins the loop as one more process through the
 * eval_model / model_next_event hooks. The normative contract, clause by
 * clause with its test map, lives in CONTRACT.md next to this file.
 *
 * The cycle, per time slot:
 *
 *   round:   run runnable processes to suspension
 *            commit staged signals and drives     (the single update point)
 *            eval the model                       (internal convergence)
 *            sample observed values, derive edges
 *            wake satisfied waits
 *            -> repeat while any process ran or any signal changed (bounded)
 *   settled: postponed sections run once
 *   advance: T = min(next timed wake, model next event)
 *
 * Header-only, C++23, no third-party dependency (the verilated model binds
 * through eval_model / model_next_event, so verilator is a consumer, not a
 * build dep). CONTRACT.md beside this header is normative for the cycle and
 * the clause semantics; each clause cites the tier-0 test that pins it.
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace veroutines {

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------

/**
 * @brief Canonical simulation time: one tick is one femtosecond.
 *
 * The typed API accepts any std::chrono duration with an exact conversion,
 * so std literals (25ns, 5us) work directly; _ps and _fs cover what the
 * std literals cannot express.
 */
using sim_time = std::chrono::duration<std::uint64_t, std::femto>;

namespace literals {
    constexpr auto operator""_fs (unsigned long long v) { return sim_time{v}; }
    constexpr auto operator""_ps (unsigned long long v) { return sim_time{v * 1000ull}; }
    constexpr auto operator""_ns (unsigned long long v) { return sim_time{v * 1000000ull}; }
    constexpr auto operator""_us (unsigned long long v) { return sim_time{v * 1000000000ull}; }
}

inline constexpr auto never = std::numeric_limits<std::uint64_t>::max();

// -----------------------------------------------------------------------------
// Intrusive run queue
// -----------------------------------------------------------------------------

namespace ilist {
    template<typename T>
    void push_back(T*& head, T*& tail, T& item) {
        item.next = nullptr;

        if (tail) {
            tail->next = &item;
            tail = &item;
        } else {
            head = tail = &item;
        }
    }

    template<typename T>
    T* pop_front(T*& head, T*& tail) {
        if (!head) {
            return nullptr;
        }

        auto* item = head;
        head = head->next;
        if (!head) {
            tail = nullptr;
        }

        return item;
    }
}

// -----------------------------------------------------------------------------
// Promise base
// -----------------------------------------------------------------------------

struct scheduler;

/**
 * @brief Shared promise state: scheduling links and ownership.
 *
 * A parked promise sits in exactly one wait structure at a time (runnable
 * queue, timed queue, condition list, settled list, or a channel). The
 * scheduler pointer is set at spawn and inherited parent to child on
 * co_await, so awaitables take no scheduler argument.
 *
 * unpark removes this promise from whatever structure holds it; each park
 * site installs it, schedule() clears it. The destructor runs it, so
 * destroying a parked frame unlinks it - and destroying a frame that owns a
 * suspended await chain unlinks the whole chain, deepest first (C12).
 */
struct promise_base {
    promise_base* next{nullptr};
    scheduler* sched{nullptr};
    std::coroutine_handle<> self{};
    std::coroutine_handle<> continuation{};
    std::exception_ptr error{};
    std::string name{};
    std::function<void()> unpark{};
    bool detached{false};
    bool queued{false};
    bool cancelled{false};

    ~promise_base();   // runs unpark unless the scheduler is tearing down
};

template<typename T = void>
struct task;

// -----------------------------------------------------------------------------
// Scheduler
// -----------------------------------------------------------------------------

/**
 * @brief The simulation kernel: run queue, time queue, delta convergence.
 *
 * run() advances time slot by slot until quiescence or stop(); run_until
 * and run_for process every slot in [now, t) and leave now == t with the
 * last slot fully settled. Detached frames are owned by the scheduler and
 * destroyed at teardown; awaited frames unwind through their owners.
 */
struct scheduler {
    // ---- Model hooks -------------------------------------------------------

    std::function<void()> eval_model;
    std::function<std::uint64_t()> model_next_event;   // absolute fs, or never
    std::function<void(std::uint64_t)> on_time_advance;

    int max_rounds{1000};

    scheduler() = default;
    scheduler(const scheduler&) = delete;
    scheduler& operator=(const scheduler&) = delete;

    ~scheduler() {
        in_teardown = true;   // parked frames unlink lazily; skip their unpark

        auto orphans = std::move(roots);
        for (auto* p : orphans) {
            p->self.destroy();
        }
    }

    // ---- Control -----------------------------------------------------------

    auto now() const { return sim_time{time_fs}; }
    void stop() { stop_requested = true; }

    void spawn(task<> t, std::string name = {});

    void run() { run_loop(never); }

    void run_until(sim_time t) {
        run_loop(t.count());

        if (!stop_requested && time_fs < t.count()) {
            time_fs = t.count();
            if (on_time_advance) {
                on_time_advance(time_fs);
            }
        }
    }

    void run_for(sim_time d) { run_until(sim_time{time_fs} + d); }

    // ---- Raw boundary hooks (model_in/model_out register through these) ----

    void register_commit(std::function<void()> fn) { commit_hooks.push_back(std::move(fn)); }
    void register_sample(std::function<void()> fn) { sample_hooks.push_back(std::move(fn)); }

    // A named signal for tracing: wave::add_all() sweeps these. A dotted name
    // (e.g. "tx.shifter") becomes a scope path.
    struct traced {
        std::string name;
        int width;
        std::function<std::uint64_t()> read;
    };

    void register_traced(std::string name, int width, std::function<std::uint64_t()> read) {
        traced_signals.push_back({std::move(name), width, std::move(read)});
    }

    std::vector<traced> traced_signals;

    // ---- Kernel internals: awaitables, ports, and channels park here -------

    std::uint64_t time_fs{0};
    std::uint64_t change_epoch{0};
    bool in_settled{false};
    bool stop_requested{false};
    bool in_teardown{false};

    void note_change() { ++change_epoch; }

    void guard_staging() {
        if (in_settled) {
            fatal("staged write from a settled section");
        }
    }

    void schedule(promise_base& p) {
        p.queued = true;
        p.unpark = nullptr;
        ilist::push_back(run_head, run_tail, p);
    }

    void park_timed(promise_base& p, std::uint64_t wake) {
        auto seq = timed_seq++;
        timed.push({wake, seq, &p});
        p.unpark = [this, seq] { timed_dead.insert(seq); };
    }

    void park_condition(promise_base& p, std::function<bool()> pred) {
        condition_waits.push_back({std::move(pred), &p});
        p.unpark = [this, &p] {
            std::erase_if(condition_waits, [&p](const condition_wait& w) { return w.taskp == &p; });
        };
    }

    void park_settled(promise_base& p) {
        settled_waiters.push_back(&p);
        p.unpark = [this, &p] { std::erase(settled_waiters, &p); };
    }

    /**
     * @brief Cancel a parked or queued process: unlink and destroy its frame.
     *
     * A queued process is flagged and destroyed when the round pops it (never
     * resumed); an otherwise-parked one is destroyed now, its destructor
     * unlinking it and its whole suspended await chain (C12).
     */
    void cancel(promise_base& p) {
        if (p.queued) {
            p.cancelled = true;
            return;
        }
        if (p.detached) {
            root_done(p);   // destroy() bypasses final_awaiter's root cleanup
        }
        p.self.destroy();
    }

    void adopt_root(promise_base& p) { roots.push_back(&p); }
    void root_done(promise_base& p) { std::erase(roots, &p); }

    [[noreturn]] void fatal(const char* what) {
        std::fprintf(stderr, "veroutines: fatal at %llu fs: %s\n",
                     static_cast<unsigned long long>(time_fs), what);
        std::abort();
    }

private:
    void run_loop(std::uint64_t limit) {
        while (!stop_requested) {
            superstep();
            if (stop_requested) {
                break;
            }

            auto next = next_event();
            if (next >= limit) {   // covers never: quiescent universe
                break;
            }
            advance_to(next);
        }
    }

    // Drop cancelled timed waits once they surface at the heap top, so a
    // cancelled far-future delay never sets the quiescence time.
    void purge_dead_top() {
        while (!timed.empty() && timed_dead.erase(timed.top().seq)) {
            timed.pop();
        }
    }

    std::uint64_t next_event() {
        purge_dead_top();

        auto t = timed.empty() ? never : timed.top().wake;
        if (model_next_event) {
            t = std::min(t, model_next_event());
        }

        return t;
    }

    void advance_to(std::uint64_t t) {
        time_fs = t;
        if (on_time_advance) {
            on_time_advance(t);
        }

        purge_dead_top();
        while (!timed.empty() && timed.top().wake <= time_fs) {
            auto entry = timed.top();
            timed.pop();

            if (timed_dead.erase(entry.seq)) {
                continue;   // cancelled: taskp is dangling, never dereference it
            }
            schedule(*entry.taskp);
        }
    }

    void superstep() {
        auto rounds = 0;

        do {
            ++rounds;
            if (rounds > max_rounds) {
                report_livelock();
            }
            round(rounds > max_rounds - 4);
        } while (run_head != nullptr || model_at_now());

        settled_phase();
    }

    void round(bool record) {
        if (record) {
            last_runners.clear();
        }

        while (auto* p = ilist::pop_front(run_head, run_tail)) {
            p->queued = false;

            if (p->cancelled) {
                if (p->detached) {
                    root_done(*p);   // destroy() bypasses final_awaiter's root cleanup
                }
                p->self.destroy();   // cancelled while queued: destroy, never resume
                continue;
            }
            if (record) {
                last_runners.push_back(p->name.empty() ? std::string{"<anonymous>"} : p->name);
            }
            p->self.resume();   // p may be gone after this: detached frames self-destroy
        }

        for (auto& commit : commit_hooks) {
            commit();
        }
        if (eval_model) {
            eval_model();
        }
        for (auto& sample : sample_hooks) {
            sample();
        }

        wake_conditions();
    }

    void wake_conditions() {
        auto it = condition_waits.begin();

        while (it != condition_waits.end()) {
            if (it->pred()) {
                schedule(*it->taskp);
                it = condition_waits.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool model_at_now() const {
        return model_next_event && model_next_event() == time_fs;
    }

    void settled_phase() {
        if (settled_waiters.empty()) {
            return;
        }

        auto batch = std::exchange(settled_waiters, {});
        in_settled = true;
        for (auto* p : batch) {
            p->self.resume();
        }
        in_settled = false;

        if (run_head != nullptr) {
            fatal("settled section scheduled same-slot work");
        }
    }

    [[noreturn]] void report_livelock() {
        std::fprintf(stderr,
                     "veroutines: time slot at %llu fs exceeded %d delta rounds;"
                     " last round ran:\n",
                     static_cast<unsigned long long>(time_fs), max_rounds);
        for (auto& n : last_runners) {
            std::fprintf(stderr, "  %s\n", n.c_str());
        }
        std::abort();
    }

    struct timed_wait {
        std::uint64_t wake;
        std::uint64_t seq;   // FIFO tie-break: equal wake times pop in push order
        promise_base* taskp;

        bool operator>(const timed_wait& o) const {
            return wake != o.wake ? wake > o.wake : seq > o.seq;
        }
    };

    struct condition_wait {
        std::function<bool()> pred;
        promise_base* taskp;
    };

    promise_base* run_head{nullptr};
    promise_base* run_tail{nullptr};
    std::priority_queue<timed_wait, std::vector<timed_wait>, std::greater<>> timed;
    std::uint64_t timed_seq{0};
    std::unordered_set<std::uint64_t> timed_dead;
    std::vector<condition_wait> condition_waits;
    std::vector<promise_base*> settled_waiters;
    std::vector<promise_base*> roots;
    std::vector<std::string> last_runners;
    std::vector<std::function<void()>> commit_hooks, sample_hooks;
};

inline promise_base::~promise_base() {
    if (unpark && !(sched && sched->in_teardown)) {
        unpark();
    }
}

// -----------------------------------------------------------------------------
// Tasks
// -----------------------------------------------------------------------------

namespace detail {
    template<typename T>
    struct task_result {
        std::optional<T> value{};

        void return_value(T v) { value.emplace(std::move(v)); }
        T take() { return std::move(*value); }
    };

    template<>
    struct task_result<void> {
        void return_void() {}
        void take() {}
    };

    template<typename P>
    auto& base_of(std::coroutine_handle<P> h) {
        return static_cast<promise_base&>(h.promise());
    }
}

/**
 * @brief A process or subroutine: coroutine carrying a value or exception.
 *
 * co_await starts the child immediately and resumes the owner on completion
 * (symmetric transfer, constant stack), delivering the value or rethrowing.
 * spawn() detaches the frame to the scheduler instead. The handle is owned
 * by whoever holds the task object; spawn and co_await both consume it.
 */
template<typename T>
struct [[nodiscard]] task {
    struct promise_type;
    using handle_t = std::coroutine_handle<promise_type>;

    explicit task(handle_t h) : h_(h) {}
    task(task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    task(const task&) = delete;
    ~task() { if (h_) h_.destroy(); }

    handle_t release() && { return std::exchange(h_, {}); }

    bool await_ready() const noexcept { return false; }

    template<typename P>
    auto await_suspend(std::coroutine_handle<P> parent) noexcept {
        auto& child = h_.promise();
        child.sched = detail::base_of(parent).sched;
        child.continuation = parent;

        return h_;   // symmetric transfer into the child
    }

    T await_resume() {
        auto& p = h_.promise();
        if (p.error) {
            std::rethrow_exception(p.error);
        }

        return p.take();
    }

private:
    handle_t h_;
};

template<typename T>
struct task<T>::promise_type : promise_base, detail::task_result<T> {
    auto get_return_object() {
        auto h = handle_t::from_promise(*this);
        self = h;
        return task{h};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(handle_t h) noexcept {
            auto& p = h.promise();

            if (p.continuation) {
                return p.continuation;
            }

            p.sched->root_done(p);
            h.destroy();
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    auto final_suspend() noexcept { return final_awaiter{}; }

    void unhandled_exception() {
        if (detached) {
            std::fprintf(stderr, "veroutines: unhandled exception in detached task '%s'\n",
                         name.empty() ? "<anonymous>" : name.c_str());
            std::terminate();
        }
        error = std::current_exception();
    }
};

inline void scheduler::spawn(task<> t, std::string name) {
    auto h = std::move(t).release();
    auto& p = h.promise();

    p.sched = this;
    p.name = std::move(name);
    p.detached = true;

    adopt_root(p);
    schedule(p);
}

// -----------------------------------------------------------------------------
// Ports and signals
// -----------------------------------------------------------------------------

/**
 * @brief Committed value with edge derivation, shared by signal, model_in,
 *        and model_out.
 *
 * prev and current shift once per delta round at the update point, so
 * rose()/fell()/changed() hold through exactly the waking round; generation
 * counts committed changes, letting a wait armed against it observe only
 * changes committed after arming.
 */
template<typename T>
struct edge_state {
    T prev{};
    T current{};
    std::uint64_t generation{0};

    bool changed() const { return current != prev; }
    bool rose() const { return static_cast<bool>(current) && !static_cast<bool>(prev); }
    bool fell() const { return !static_cast<bool>(current) && static_cast<bool>(prev); }

    operator T() const { return current; }
    T get() const { return current; }
};

/**
 * @brief Pure-cosim signal: staged write, committed once per delta round.
 *
 * sig <<= v is the only write; reads convert to the last committed value.
 * The last stage in a round wins. Staging from a settled section is a
 * fatal error.
 */
template<typename T>
struct signal : edge_state<T> {
    explicit signal(scheduler& s, T init = T{}) : sched(&s), pending(init) {
        this->prev = init;
        this->current = init;
        s.register_commit([this] { commit(); });
    }

    // Named: also self-registers for wave::add_all(). The name is the signal's
    // trace identity (a dotted name nests into scopes). Taken as const char* so
    // a string-literal name is an exact match, not confused with the init value.
    signal(scheduler& s, const char* name, T init = T{}) : signal(s, init) {
        s.register_traced(name,
                          std::is_same_v<T, bool> ? 1 : int(sizeof(T) * 8),
                          [this] { return static_cast<std::uint64_t>(this->get()); });
    }

    signal(const signal&) = delete;
    signal& operator=(const signal&) = delete;
    template<typename U> signal& operator=(U&&) = delete;   // <<= is the write

    signal& operator<<=(T v) {
        sched->guard_staging();
        pending = std::move(v);
        return *this;
    }

private:
    void commit() {
        this->prev = this->current;

        if (pending != this->current) {
            this->current = pending;
            ++this->generation;
            sched->note_change();
        }
    }

    scheduler* sched;
    T pending;
};

/**
 * @brief A model input port: a staged write bound to a model input, committed
 *        before eval. Named from the model's side - the testbench drives it.
 *
 * Bind a sink functor for packed or wide destinations, or a plain pointer.
 * The sink runs on committed changes, so the model field must start equal to
 * this port's initial value. The committed value carries the edge API, so
 * processes can wait on the port itself (e.g. a generated clock).
 */
template<typename T>
struct model_in : edge_state<T> {
    model_in() = default;
    model_in(scheduler& s, T* ptr) { bind(s, ptr); }
    model_in(const model_in&) = delete;
    model_in& operator=(const model_in&) = delete;
    template<typename U> model_in& operator=(U&&) = delete;

    template<typename Sink>
    void bind(scheduler& s, Sink write) {
        sched = &s;
        sink = std::move(write);
        s.register_commit([this] { commit(); });
    }

    void bind(scheduler& s, T* ptr) {
        bind(s, [ptr](T v) { *ptr = v; });
    }

    model_in& operator<<=(T v) {
        sched->guard_staging();
        pending = std::move(v);
        return *this;
    }

private:
    void commit() {
        this->prev = this->current;

        if (pending != this->current) {
            this->current = pending;
            ++this->generation;
            sched->note_change();
            if (sink) {
                sink(this->current);
            }
        }
    }

    scheduler* sched{nullptr};
    std::function<void(T)> sink;
    T pending{};
};

/**
 * @brief A model output port: sampled once per delta round, after eval. Named
 *        from the model's side - the testbench reads it, and cannot write it.
 *
 * Bind a source functor for packed or wide origins, or construct with a plain
 * pointer.
 */
template<typename T>
struct model_out : edge_state<T> {
    model_out() = default;
    model_out(scheduler& s, const T* ptr) { bind(s, ptr); }
    model_out(const model_out&) = delete;
    model_out& operator=(const model_out&) = delete;

    template<typename Source>
    void bind(scheduler& s, Source read) {
        sched = &s;
        source = std::move(read);
        s.register_sample([this] { sample(); });
    }

    void bind(scheduler& s, const T* ptr) {
        bind(s, [ptr] { return *ptr; });
    }

    void bind(scheduler& s, T* ptr) {
        bind(s, static_cast<const T*>(ptr));
    }

private:
    void sample() {
        this->prev = this->current;

        auto v = source();
        if (v != this->current) {
            this->current = v;
            ++this->generation;
            sched->note_change();
        }
    }

    scheduler* sched{nullptr};
    std::function<T()> source;
};

// -----------------------------------------------------------------------------
// Waits
// -----------------------------------------------------------------------------

struct delay_awaiter {
    std::uint64_t ticks;

    bool await_ready() const noexcept { return ticks == 0; }

    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) const {
        auto& p = detail::base_of(h);
        p.sched->park_timed(p, p.sched->time_fs + ticks);
    }

    void await_resume() const noexcept {}
};

/**
 * @brief Suspend until d of simulation time has passed.
 */
inline auto delay(sim_time d) { return delay_awaiter{d.count()}; }

/**
 * @brief Durations await directly: co_await 25_ns is delay(25_ns).
 */
inline auto operator co_await(sim_time d) { return delay_awaiter{d.count()}; }

template<typename Pred>
struct until_awaiter {
    Pred pred;

    bool await_ready() { return pred(); }

    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) {
        auto& p = detail::base_of(h);
        p.sched->park_condition(p, std::move(pred));
    }

    void await_resume() const noexcept {}
};

/**
 * @brief Suspend until pred returns true; polled once per delta round.
 *
 * The polled form is the correct wait for plain C++ state. State mutated
 * in a settled section is observed from the following slot.
 */
template<typename Pred>
auto until(Pred pred) { return until_awaiter<Pred>{std::move(pred)}; }

struct settled_awaiter {
    bool await_ready() const noexcept { return false; }

    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) const {
        auto& p = detail::base_of(h);
        p.sched->park_settled(p);
    }

    void await_resume() const noexcept {}
};

/**
 * @brief Suspend until the current slot's delta loop converges.
 *
 * Resumes once per slot, reading final state; a settled section must
 * leave the slot quiescent and may suspend only on a future event.
 */
inline auto settled() { return settled_awaiter{}; }

namespace detail {
    enum class edge { rise, fall };

    template<typename S>
    struct edge_awaiter {
        S& sig;
        edge kind;

        bool await_ready() const noexcept { return false; }

        template<typename P>
        void await_suspend(std::coroutine_handle<P> h) const {
            auto& p = base_of(h);
            auto* s = &sig;
            auto seen = sig.generation;
            auto want_rise = kind == edge::rise;

            p.sched->park_condition(p, [s, seen, want_rise] {
                if (s->generation <= seen) {
                    return false;
                }
                return static_cast<bool>(s->get()) == want_rise;
            });
        }

        void await_resume() const noexcept {}
    };
}

/**
 * @brief Wait for a rising edge committed after arming.
 */
template<typename S>
auto posedge(S& sig) { return detail::edge_awaiter<S>{sig, detail::edge::rise}; }

/**
 * @brief Wait for a falling edge committed after arming.
 */
template<typename S>
auto negedge(S& sig) { return detail::edge_awaiter<S>{sig, detail::edge::fall}; }

/**
 * @brief Wait for any committed change on the listed signals - the
 *        explicit sensitivity list.
 */
template<typename... S>
auto changed(S&... sigs) {
    return until([&sigs..., seen = std::array{sigs.generation...}] {
        auto i = std::size_t{0};
        return ((sigs.generation > seen[i++]) || ...);
    });
}

struct any_t {};
inline constexpr any_t any{};

struct any_changed_awaiter {
    bool await_ready() const noexcept { return false; }

    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) const {
        auto& p = detail::base_of(h);
        auto* s = p.sched;
        auto seen = s->change_epoch;

        s->park_condition(p, [s, seen] { return s->change_epoch > seen; });
    }

    void await_resume() const noexcept {}
};

/**
 * @brief Conservative wildcard sensitivity: wake on any committed change.
 *
 * Re-staging an unchanged value is not a change, so pure re-stagers
 * converge.
 */
inline auto changed(any_t) { return any_changed_awaiter{}; }

// -----------------------------------------------------------------------------
// Channels
// -----------------------------------------------------------------------------

/**
 * @brief Unbuffered rendezvous channel; parked parties served in arrival
 *        order.
 *
 * close() resumes every parked party - recv yields nullopt, send yields
 * false - and later operations complete immediately the same way.
 * Cancellation flows through these results.
 */
template<typename T>
struct channel {
    explicit channel(scheduler& s) : sched(&s) {}
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    struct send_awaiter {
        channel* ch;
        T value;
        promise_base* me{nullptr};
        bool ok{false};

        bool await_ready() const noexcept { return false; }

        template<typename P>
        bool await_suspend(std::coroutine_handle<P> h) {
            auto& p = detail::base_of(h);

            if (ch->closed) {
                ok = false;
                return false;
            }
            if (!ch->receivers.empty()) {
                auto* r = ch->receivers.front();
                ch->receivers.pop_front();

                r->result = std::move(value);
                ch->sched->schedule(*r->me);
                ok = true;
                return false;
            }

            me = &p;
            ch->senders.push_back(this);
            p.unpark = [c = ch, aw = this] { std::erase(c->senders, aw); };
            return true;
        }

        bool await_resume() const noexcept { return ok; }
    };

    struct recv_awaiter {
        channel* ch;
        promise_base* me{nullptr};
        std::optional<T> result{};

        bool await_ready() const noexcept { return false; }

        template<typename P>
        bool await_suspend(std::coroutine_handle<P> h) {
            auto& p = detail::base_of(h);

            if (!ch->senders.empty()) {
                auto* s = ch->senders.front();
                ch->senders.pop_front();

                result = std::move(s->value);
                s->ok = true;
                ch->sched->schedule(*s->me);
                return false;
            }
            if (ch->closed) {
                result.reset();
                return false;
            }

            me = &p;
            ch->receivers.push_back(this);
            p.unpark = [c = ch, aw = this] { std::erase(c->receivers, aw); };
            return true;
        }

        std::optional<T> await_resume() { return std::move(result); }
    };

    auto send(T v) { return send_awaiter{this, std::move(v)}; }
    auto recv() { return recv_awaiter{this}; }

    void close() {
        closed = true;

        auto ss = std::exchange(senders, {});
        for (auto* s : ss) {
            s->ok = false;
            sched->schedule(*s->me);
        }

        auto rs = std::exchange(receivers, {});
        for (auto* r : rs) {
            r->result.reset();
            sched->schedule(*r->me);
        }
    }

    scheduler* sched;
    std::deque<send_awaiter*> senders;
    std::deque<recv_awaiter*> receivers;
    bool closed{false};
};

// -----------------------------------------------------------------------------
// Race
// -----------------------------------------------------------------------------

namespace detail {
    /**
     * @brief Hand the running process its own promise without suspending.
     */
    struct self_promise {
        promise_base* p{nullptr};

        bool await_ready() const noexcept { return false; }

        template<typename P>
        bool await_suspend(std::coroutine_handle<P> h) {
            p = &base_of(h);
            return false;   // resume immediately
        }

        promise_base* await_resume() const noexcept { return p; }
    };

    struct race_state {
        std::optional<std::size_t> winner{};
        std::vector<promise_base*> racers;   // slot i cleared once runner i finishes
    };

    template<typename A>
    task<> race_runner(race_state& st, std::size_t i, A aw) {
        st.racers[i] = co_await self_promise{};
        co_await std::move(aw);

        if (!st.winner) {
            st.winner = i;
        }
        st.racers[i] = nullptr;
    }

    /**
     * @brief Cancel every still-live racer on scope exit; runs on the
     *        coordinator's normal return and on its own cancellation alike.
     */
    struct race_guard {
        scheduler* sched;
        std::vector<promise_base*>* racers;

        ~race_guard() {
            for (auto* r : *racers) {
                if (r) {
                    sched->cancel(*r);
                }
            }
        }
    };
}

/**
 * @brief Run awaitables concurrently; resume with the index of the first to
 *        finish and cancel the rest.
 *
 * race(work(), delay(d)) is the timeout idiom: 0 means work finished, 1 means
 * it timed out. Ties within a delta round resolve by argument order. A loser's
 * frame - and any await chain it owns - is destroyed at the winner's time.
 */
template<typename... As>
task<std::size_t> race(As... aws) {
    detail::race_state st;
    st.racers.resize(sizeof...(As));

    auto* self = co_await detail::self_promise{};
    auto* sched = self->sched;

    auto i = std::size_t{0};
    (sched->spawn(detail::race_runner(st, i++, std::move(aws)), "race-runner"), ...);

    detail::race_guard guard{sched, &st.racers};

    co_await until([&st] { return st.winner.has_value(); });
    co_return *st.winner;
}

// -----------------------------------------------------------------------------
// HDL surface
// -----------------------------------------------------------------------------
//
// VHDL-flavored sugar over spawn and the wait primitives - nothing the kernel
// does not already expose. A clocked process waits on an edge; a combinational
// process re-evaluates on any change and converges by the fixpoint, so it needs
// no sensitivity list (C5).

/**
 * @brief A clock edge as a reusable trigger: co_await rising_edge(clk) directly,
 *        or pass it to process(). Re-arming rebuilds the underlying edge wait,
 *        so a process loop reuses one trigger across cycles.
 */
template<typename S>
struct edge_trigger {
    S* sig;
    detail::edge kind;

    bool await_ready() const noexcept { return false; }

    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) const {
        detail::edge_awaiter<S>{*sig, kind}.await_suspend(h);
    }

    void await_resume() const noexcept {}
};

template<typename S>
edge_trigger<S> rising_edge(S& sig) { return {&sig, detail::edge::rise}; }

template<typename S>
edge_trigger<S> falling_edge(S& sig) { return {&sig, detail::edge::fall}; }

namespace detail {
    template<typename Trig, typename F>
    task<> triggered_process(Trig trig, F body) {
        static_assert(std::is_void_v<decltype(body())>,
                      "process body must be synchronous; write an explicit task<> loop "
                      "for a body that co_awaits");
        for (;;) {
            co_await trig;
            body();
        }
    }

    template<typename F>
    task<> comb_process(F body) {
        static_assert(std::is_void_v<decltype(body())>,
                      "comb body must be synchronous - it re-runs to a fixpoint (C5)");
        for (;;) {
            body();
            co_await changed(any);
        }
    }
}

/**
 * @brief A triggered process: runs a synchronous body once each time the
 *        trigger fires.
 *
 * The trigger is any re-armable awaitable - an edge (rising_edge / falling_edge)
 * for a clocked process, or a delay for a periodic one. The body reads committed
 * state and stages the next values; a body that must co_await belongs in an
 * explicit task<> loop, where its suspends stay visible.
 */
template<typename Trig, typename F>
void process(scheduler& s, Trig trig, F body, std::string name = {}) {
    s.spawn(detail::triggered_process(std::move(trig), std::move(body)), std::move(name));
}

/**
 * @brief A concurrent combinational statement: a synchronous body re-evaluates
 *        on any change and converges by the fixpoint, so it carries no
 *        sensitivity list.
 */
template<typename F>
void comb(scheduler& s, F body, std::string name = {}) {
    s.spawn(detail::comb_process(std::move(body)), std::move(name));
}

// -----------------------------------------------------------------------------
// Waveform tracing
// -----------------------------------------------------------------------------
//
// A settled-phase VCD tracer: one file, written live, carrying C++ signals and
// any model signal surfaced through model_out - so a cosim trace holds both
// heads with no post-merge. It samples at settled() (the slot's converged
// state), so the file shows one value per time slot, not delta transients.

/**
 * @brief Unified VCD tracer. Register signals with add() before run(); values
 *        are sampled once per slot in the settled phase and written on change.
 *
 * Traces anything with the port value interface (signal, model_in, model_out).
 * Widths default to the type size (bool is one bit); pass width for a packed
 * field. Up to 64 bits per signal.
 */
struct wave {
    wave(scheduler& s, const char* path, std::string top = "top");
    ~wave() { if (file) { std::fclose(file); } }

    wave(const wave&) = delete;
    wave& operator=(const wave&) = delete;

    void scope(const std::string& name) { decls += "$scope module " + name + " $end\n"; }
    void pop() { decls += "$upscope $end\n"; }

    template<typename T>
    wave& add(const std::string& name, const edge_state<T>& sig, int width = -1) {
        int w = width > 0 ? width : (std::is_same_v<T, bool> ? 1 : int(sizeof(T) * 8));
        auto code = id(entries.size());

        decls += "$var wire " + std::to_string(w) + " " + code + " " + name + " $end\n";
        entries.push_back({code, w, [&sig] { return static_cast<std::uint64_t>(sig.get()); }, 0});
        return *this;
    }

    // Add every named signal (signal<T>{s, "name"}). A dotted name nests into
    // scopes, so "tx.shifter" lands in a tx scope. Call before run().
    wave& add_all() {
        auto sigs = sched->traced_signals;
        std::sort(sigs.begin(), sigs.end(),
                  [](const scheduler::traced& a, const scheduler::traced& b) { return a.name < b.name; });

        std::vector<std::string> stack;
        for (auto& t : sigs) {
            std::vector<std::string> path;
            std::size_t start = 0;
            for (std::size_t i = 0; i <= t.name.size(); i++) {
                if (i == t.name.size() || t.name[i] == '.') {
                    path.push_back(t.name.substr(start, i - start));
                    start = i + 1;
                }
            }
            std::string leaf = path.back();
            path.pop_back();

            std::size_t common = 0;
            while (common < stack.size() && common < path.size() && stack[common] == path[common]) {
                common++;
            }
            for (std::size_t i = stack.size(); i > common; i--) {
                decls += "$upscope $end\n";
            }
            stack.resize(common);
            for (std::size_t i = common; i < path.size(); i++) {
                decls += "$scope module " + path[i] + " $end\n";
                stack.push_back(path[i]);
            }

            auto code = id(entries.size());
            decls += "$var wire " + std::to_string(t.width) + " " + code + " " + leaf + " $end\n";
            entries.push_back({code, t.width, t.read, 0});
        }
        for (std::size_t i = stack.size(); i > 0; i--) {
            decls += "$upscope $end\n";
        }
        return *this;
    }

    void dump() {
        if (!file) {
            return;
        }

        auto t = sched->now().count();
        if (!header_done) {
            std::fprintf(file, "$timescale 1fs $end\n$scope module %s $end\n%s$upscope $end\n"
                               "$enddefinitions $end\n#%llu\n$dumpvars\n",
                         top_name.c_str(), decls.c_str(), static_cast<unsigned long long>(t));
            for (auto& e : entries) {
                e.last = e.read();
                emit(e);
            }
            std::fprintf(file, "$end\n");
            header_done = true;
            return;
        }

        bool stamped = false;
        for (auto& e : entries) {
            auto v = e.read();
            if (v == e.last) {
                continue;
            }
            if (!stamped) {
                std::fprintf(file, "#%llu\n", static_cast<unsigned long long>(t));
                stamped = true;
            }
            e.last = v;
            emit(e);
        }
    }

private:
    struct entry {
        std::string code;
        int width;
        std::function<std::uint64_t()> read;
        std::uint64_t last;
    };

    void emit(const entry& e) {
        if (e.width == 1) {
            std::fprintf(file, "%d%s\n", int(e.last & 1), e.code.c_str());
        } else {
            std::string bits;
            for (int b = e.width - 1; b >= 0; b--) {
                bits.push_back(((e.last >> b) & 1) ? '1' : '0');
            }
            std::fprintf(file, "b%s %s\n", bits.c_str(), e.code.c_str());
        }
    }

    static std::string id(std::size_t i) {
        std::string c;
        do {
            c.push_back(static_cast<char>(33 + (i % 94)));
            i /= 94;
        } while (i > 0);
        return c;
    }

    scheduler* sched;
    std::FILE* file;
    std::string top_name;
    std::string decls;
    std::vector<entry> entries;
    bool header_done{false};
};

namespace detail {
    inline task<> wave_proc(wave& w) {
        for (;;) {
            co_await settled();
            w.dump();
        }
    }
}

inline wave::wave(scheduler& s, const char* path, std::string top)
    : sched(&s), file(std::fopen(path, "w")), top_name(std::move(top)) {
    s.spawn(detail::wave_proc(*this), "wave");
}

}
