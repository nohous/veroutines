#pragma once

#include <vector>
#include <queue>
#include <functional>
#include <cstdint>
#include <memory>
#include <iostream>
#include <verilated.h>

namespace Veroutines {

class Scheduler;

// -----------------------------------------------------------------------------
// Observable - Base for dependency tracking and type erasure
// -----------------------------------------------------------------------------

class Observable {
    friend class Scheduler;
protected:
    std::vector<size_t> dependents_;

public:
    virtual ~Observable() = default;
    virtual void commit() {}
    virtual void sample() {}
    virtual bool dirty() const { return false; }
    virtual bool changed() const = 0;

    void add_dependent(size_t pid) { dependents_.push_back(pid); }
    const std::vector<size_t>& dependents() const { return dependents_; }
};

// -----------------------------------------------------------------------------
// InputPort<T> - Boundary: Testbench -> DUT
//
// Buffers writes until commit, then applies to DUT.
// Tracks edges so processes can react to C++-driven signals (e.g. clock).
// -----------------------------------------------------------------------------

template<typename T>
class InputPort : public Observable {
    T* ptr_;       // -> DUT input
    T staged_;     // Buffered write, applied on commit
    T value_;      // Committed value (what DUT sees)
    T before_;     // Value before last commit (for edges)
    bool dirty_ = false;

public:
    explicit InputPort(T* ptr)
        : ptr_(ptr), staged_(*ptr), value_(*ptr), before_(*ptr) {}

    void write(T v) { staged_ = v; dirty_ = true; }

    void commit() override {
        before_ = value_;
        if (dirty_) {
            *ptr_ = staged_;
            value_ = staged_;
            dirty_ = false;
        }
    }

    bool dirty() const override { return dirty_; }
    bool changed() const override { return value_ != before_; }
    bool posedge() const { return !before_ && value_; }
    bool negedge() const { return before_ && !value_; }

    T val() const { return value_; }
    operator T() const { return value_; }
};

// -----------------------------------------------------------------------------
// OutputPort<T> - Boundary: DUT -> Testbench
//
// Samples DUT output each delta. Read-only from testbench.
// Tracks edges for process triggering.
// -----------------------------------------------------------------------------

template<typename T>
class OutputPort : public Observable {
    T* ptr_;       // -> DUT output
    T value_;      // Current sampled value
    T before_;     // Value before last sample (for edges)

public:
    explicit OutputPort(T* ptr)
        : ptr_(ptr), value_(*ptr), before_(*ptr) {}

    void sample() override {
        before_ = value_;
        value_ = *ptr_;
    }

    bool changed() const override { return value_ != before_; }
    bool posedge() const { return !before_ && value_; }
    bool negedge() const { return before_ && !value_; }

    T val() const { return value_; }
    operator T() const { return value_; }
};

// -----------------------------------------------------------------------------
// Signal<T> - Internal testbench state with NBA semantics
//
// Writes buffered until commit. Enables derived clocks, state machines.
// -----------------------------------------------------------------------------

template<typename T>
class Signal : public Observable {
    T staged_;     // Buffered write, applied on commit
    T value_;      // Committed value
    T before_;     // Value before last commit (for edges)
    bool dirty_ = false;

public:
    explicit Signal(T initial = T{})
        : staged_(initial), value_(initial), before_(initial) {}

    void write(T v) { staged_ = v; dirty_ = true; }

    void commit() override {
        before_ = value_;
        if (dirty_) {
            value_ = staged_;
            dirty_ = false;
        }
    }

    bool dirty() const override { return dirty_; }
    bool changed() const override { return value_ != before_; }
    bool posedge() const { return !before_ && value_; }
    bool negedge() const { return before_ && !value_; }

    T val() const { return value_; }
    operator T() const { return value_; }

    Signal& operator=(T v) { write(v); return *this; }
};

// -----------------------------------------------------------------------------
// Process - Callback triggered by signal changes
// -----------------------------------------------------------------------------

struct Process {
    using Callback = std::function<void(Scheduler&)>;
    Callback callback;
    bool always_active;  // Run every delta regardless of triggers
};

// -----------------------------------------------------------------------------
// Scheduler - 5-phase execution kernel
//
// 1. COMMIT   - Apply staged writes, capture edges
// 2. EVAL     - model.eval()
// 3. SAMPLE   - Capture DUT outputs
// 4. REACT    - Trigger and run processes
// 5. CONVERGE - Loop if dirty, else advance time
// -----------------------------------------------------------------------------

class Scheduler {
public:
    using Action = std::function<void()>;

private:
    struct TimedEvent {
        uint64_t time;
        Action action;
        bool operator>(const TimedEvent& o) const { return time > o.time; }
    };
    std::priority_queue<TimedEvent, std::vector<TimedEvent>, std::greater<>> time_events_;

    std::vector<std::unique_ptr<Observable>> owned_;
    std::vector<Observable*> inputs_;
    std::vector<Observable*> outputs_;
    std::vector<Observable*> signals_;

    std::vector<Process> processes_;
    std::vector<bool> triggered_;

    uint64_t current_time_ = 0;

public:

    // --- Registration ---

    template<typename T>
    InputPort<T>* input(T* dut_ptr) {
        auto p = std::make_unique<InputPort<T>>(dut_ptr);
        auto* h = p.get();
        inputs_.push_back(h);
        owned_.push_back(std::move(p));
        return h;
    }

    template<typename T>
    OutputPort<T>* output(T* dut_ptr) {
        auto p = std::make_unique<OutputPort<T>>(dut_ptr);
        auto* h = p.get();
        outputs_.push_back(h);
        owned_.push_back(std::move(p));
        return h;
    }

    template<typename T>
    Signal<T>* signal(T initial = T{}) {
        auto p = std::make_unique<Signal<T>>(initial);
        auto* h = p.get();
        signals_.push_back(h);
        owned_.push_back(std::move(p));
        return h;
    }

    void process(std::initializer_list<Observable*> sens, Process::Callback cb) {
        size_t pid = processes_.size();
        processes_.push_back({std::move(cb), false});
        triggered_.push_back(false);
        for (auto* obs : sens)
            obs->add_dependent(pid);
    }

    void always(Process::Callback cb) {
        processes_.push_back({std::move(cb), true});
        triggered_.push_back(false);
    }

    // --- Scheduling ---

    uint64_t time() const { return current_time_; }

    void schedule_after(uint64_t delay, Action action) {
        time_events_.push({current_time_ + delay, std::move(action)});
    }

    void schedule_at(uint64_t t, Action action) {
        time_events_.push({t, std::move(action)});
    }

    // --- Main Loop ---

    template<typename TopModel, typename Trace = int>
    void run(VerilatedContext* ctx, TopModel* top, Trace* tfp = nullptr,
             uint64_t timeout = UINT64_MAX) {

        if (tfp) tfp->dump(0);

        while (!ctx->gotFinish() && ctx->time() < timeout) {

            // Time advancement
            uint64_t t_cosim = time_events_.empty() ? UINT64_MAX : time_events_.top().time;
            uint64_t t_model = top->eventsPending() ? top->nextTimeSlot() : UINT64_MAX;
            uint64_t t_next = std::min(t_cosim, t_model);
            if (t_next == UINT64_MAX) break;

            ctx->time(t_next);
            current_time_ = t_next;

            // Fire timed events (may stage writes)
            while (!time_events_.empty() && time_events_.top().time == current_time_) {
                auto ev = std::move(const_cast<TimedEvent&>(time_events_.top()));
                time_events_.pop();
                ev.action();
            }

            // Delta convergence loop
            int delta = 0;
            while (true) {

                // PHASE 1: COMMIT
                for (auto* p : inputs_)  p->commit();
                for (auto* p : signals_) p->commit();

                // PHASE 2: EVAL
                bool need_eval = has_dirty_input() || has_dirty_signal() ||
                                 (top->eventsPending() && top->nextTimeSlot() <= t_next);
                if (need_eval || delta == 0)
                    top->eval();

                // PHASE 3: SAMPLE
                for (auto* p : outputs_) p->sample();

                // PHASE 4: REACT
                std::fill(triggered_.begin(), triggered_.end(), false);

                for (auto* o : inputs_)
                    if (o->changed())
                        for (size_t pid : o->dependents()) triggered_[pid] = true;

                for (auto* o : signals_)
                    if (o->changed())
                        for (size_t pid : o->dependents()) triggered_[pid] = true;

                for (auto* o : outputs_)
                    if (o->changed())
                        for (size_t pid : o->dependents()) triggered_[pid] = true;

                for (size_t i = 0; i < processes_.size(); ++i)
                    if (triggered_[i] || processes_[i].always_active)
                        processes_[i].callback(*this);

                // PHASE 5: CONVERGENCE
                if (!has_dirty_input() && !has_dirty_signal())
                    break;

                if (++delta > 1000) {
                    std::cerr << "[Veroutines] Combinational loop at t=" << t_next << "\n";
                    return;
                }
            }

            if (tfp) tfp->dump(ctx->time());
        }
    }

private:
    bool has_dirty_input() const {
        for (auto* p : inputs_) if (p->dirty()) return true;
        return false;
    }

    bool has_dirty_signal() const {
        for (auto* p : signals_) if (p->dirty()) return true;
        return false;
    }
};

} // namespace Veroutines
