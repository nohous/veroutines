#pragma once

#include <vector>
#include <queue>
#include <functional>
#include <cstdint>
#include <memory>
#include <iostream>
#include <verilated.h>

namespace Veroutines {

// ============================================================================
// Signal Handle and Template Implementation (Unchanged)
// ============================================================================
class SignalBase {
    std::vector<size_t> dependent_processes_;
public:
    virtual ~SignalBase() = default;
    virtual void snapshot() = 0;
    virtual bool changed() const = 0;
    void add_dependency(size_t process_id) { dependent_processes_.push_back(process_id); }
    const std::vector<size_t>& get_dependents() const { return dependent_processes_; }
};

template <typename T>
class Signal : public SignalBase {
    T* ptr_;
    T  prev_val_;
    T  curr_val_;
public:
    explicit Signal(T* signal_ptr) : ptr_(signal_ptr) {
        curr_val_ = *ptr_;
        prev_val_ = *ptr_;
    }
    void snapshot() override {
        prev_val_ = curr_val_;
        curr_val_ = *ptr_;
    }
    inline bool changed() const override { return curr_val_ != prev_val_; }
    inline bool posedge() const { return (prev_val_ == 0) && (curr_val_ != 0); }
    inline bool negedge() const { return (prev_val_ != 0) && (curr_val_ == 0); }
    inline T val() const { return curr_val_; }
    inline T prev() const { return prev_val_; }
};

// ============================================================================
// The Scheduler
// ============================================================================
class Scheduler {
public:
    using Action = std::function<void()>;
    using TriggerCheck = std::function<void(Scheduler&)>;

private:
    // Time Wheel
    struct TimerEvent {
        uint64_t time;
        Action action;
        bool operator>(const TimerEvent& other) const { return time > other.time; }
    };
    std::priority_queue<TimerEvent, std::vector<TimerEvent>, std::greater<>> time_events_;

    // Write Buffer
    std::vector<Action> write_buffer_;

    // Process & Signal Management
    struct Process {
        TriggerCheck callback;
        bool is_always_active;
    };
    std::vector<Process> processes_;
    std::vector<bool> process_trigger_flags_; 
    std::vector<std::unique_ptr<SignalBase>> tracked_signals_;

    uint64_t current_time_ = 0;

public:
    // --- Registration API ---

    template <typename T>
    Signal<T>* register_signal(T* signal_ptr) {
        auto sig = std::make_unique<Signal<T>>(signal_ptr);
        Signal<T>* handle = sig.get();
        tracked_signals_.push_back(std::move(sig));
        return handle;
    }

    void add_sensitive_process(const std::initializer_list<SignalBase*>& sensitivity_list, TriggerCheck cb) {
        size_t pid = processes_.size();
        processes_.push_back({std::move(cb), false});
        process_trigger_flags_.push_back(false);
        for (auto* sig : sensitivity_list) sig->add_dependency(pid);
    }

    void add_sensitive_process(TriggerCheck cb) {
        processes_.push_back({std::move(cb), true});
        process_trigger_flags_.push_back(false);
    }

    // --- Scheduling API ---

    uint64_t time() const { return current_time_; }

    void schedule_after(uint64_t delay, Action action) {
        time_events_.push({current_time_ + delay, std::move(action)});
    }

    template <typename T, typename U>
    void schedule_write(T* signal_ptr, U value) {
        write_buffer_.push_back([signal_ptr, value]() { *signal_ptr = value; });
    }

private:
    // --- Internal Helpers ---

    uint64_t next_event_time() const {
        return time_events_.empty() ? UINT64_MAX : time_events_.top().time;
    }

    void advance_time(uint64_t next_time) {
        current_time_ = next_time;
        while (!time_events_.empty() && time_events_.top().time == current_time_) {
            auto ev = std::move(time_events_.top());
            time_events_.pop();
            ev.action(); // Likely schedules writes to buffer
        }
    }

    // Phase 1: Apply NBA Writes
    bool apply_nba_writes() {
        if (write_buffer_.empty()) return false;
        for (const auto& write_op : write_buffer_) write_op();
        write_buffer_.clear();
        return true;
    }

    // Phase 3: Trigger Reactivity
    void trigger_reactivity() {
        std::fill(process_trigger_flags_.begin(), process_trigger_flags_.end(), false);
        
        // 1. Snapshot & Check Changes
        for (auto& sig : tracked_signals_) {
            sig->snapshot();
            if (sig->changed()) {
                for (size_t pid : sig->get_dependents()) {
                    process_trigger_flags_[pid] = true;
                }
            }
        }

        // 2. Run Triggered Processes
        for (size_t i = 0; i < processes_.size(); ++i) {
            if (process_trigger_flags_[i] || processes_[i].is_always_active) {
                processes_[i].callback(*this);
            }
        }
    }

public:
    // ========================================================================
    // THE KERNEL LOOP (Refactored per Expert Feedback)
    // ========================================================================
    template <typename TopModel, typename Trace = int>
    void run(VerilatedContext* ctx, TopModel* top, Trace* tfp = nullptr, uint64_t timeout = UINT64_MAX) {
        if (tfp) tfp->dump(0);

        while (!ctx->gotFinish() && ctx->time() < timeout) {
            
            // 1. Time Arbitration
            uint64_t next_cpp = next_event_time();
            uint64_t next_model = top->eventsPending() ? top->nextTimeSlot() : UINT64_MAX;
            uint64_t t_next = std::min(next_cpp, next_model);

            if (t_next == UINT64_MAX) break;
            
            ctx->time(t_next);
            advance_time(t_next); // Populates write_buffer_ from timed events

            // 2. The Delta Cycle (The "Zero-Time Handshake" Loop)
            bool stable = false;
            int delta_count = 0;

            while (!stable) {
                // A. NBA PHASE: Apply writes (from Time Phase or Previous Delta)
                bool writes_applied = apply_nba_writes();

                // B. ACTIVE PHASE: Evaluate Model
                // Run eval if inputs changed (writes_applied) OR internal model events exist
                if (writes_applied || (top->eventsPending() && top->nextTimeSlot() <= t_next)) {
                    top->eval();
                }

                // C. REACTIVE PHASE: Check Monitors
                // This might populate write_buffer_ again (Combinatorial Feedback)
                trigger_reactivity();

                // D. CONVERGENCE CHECK
                // Stable if no new writes were scheduled
                if (write_buffer_.empty()) {
                    stable = true;
                } else {
                    // Loop continues to apply new writes in Phase A
                    if (++delta_count > 1000) {
                        std::cerr << "[Fatal] Combinatorial Loop at time " << t_next << std::endl;
                        return;
                    }
                }
            }
            
            // 3. ReadOnly Phase
            if (tfp) tfp->dump(ctx->time());
        }
    }
};
} // namespace Veroutines