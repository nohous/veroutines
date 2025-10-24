#include <queue>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstdint>

// Base class for type erasure
class SignalTracker {
public:
    virtual ~SignalTracker() = default;
    virtual void sample() = 0;
    virtual bool changed() const = 0;
};

// Templated implementation for specific signal types
template<typename T>
class TypedSignalTracker : public SignalTracker {
    T* signal_ptr_;
    T prev_value_;
    T curr_value_;
    
public:
    explicit TypedSignalTracker(T* ptr) 
        : signal_ptr_(ptr)
        , prev_value_(*ptr)
        , curr_value_(*ptr) {}
    
    void sample() override {
        prev_value_ = curr_value_;
        curr_value_ = *signal_ptr_;
    }
    
    bool changed() const override {
        return prev_value_ != curr_value_;
    }
    
    bool posedge() const {
        return prev_value_ == T{0} && curr_value_ != T{0};
    }
    
    bool negedge() const {
        return prev_value_ != T{0} && curr_value_ == T{0};
    }
    
    T prev_value() const { return prev_value_; }
    T curr_value() const { return curr_value_; }
};

// Signals snapshot - provides query interface to callbacks
class Signals {
    std::unordered_map<void*, SignalTracker*> trackers_;
    
public:
    explicit Signals(const std::unordered_map<void*, std::unique_ptr<SignalTracker>>& all_trackers) {
        // Build lookup map (non-owning pointers)
        for (const auto& [ptr, tracker] : all_trackers) {
            trackers_[ptr] = tracker.get();
        }
    }
    
    template<typename T>
    bool changed(T* sig) const {
        auto it = trackers_.find(static_cast<void*>(sig));
        if (it == trackers_.end()) return false;
        return it->second->changed();
    }
    
    template<typename T>
    bool posedge(T* sig) const {
        auto it = trackers_.find(static_cast<void*>(sig));
        if (it == trackers_.end()) return false;
        auto* typed = dynamic_cast<TypedSignalTracker<T>*>(it->second);
        return typed && typed->posedge();
    }
    
    template<typename T>
    bool negedge(T* sig) const {
        auto it = trackers_.find(static_cast<void*>(sig));
        if (it == trackers_.end()) return false;
        auto* typed = dynamic_cast<TypedSignalTracker<T>*>(it->second);
        return typed && typed->negedge();
    }
    
    template<typename T>
    T prev_value(T* sig) const {
        auto it = trackers_.find(static_cast<void*>(sig));
        if (it == trackers_.end()) return T{};
        auto* typed = dynamic_cast<TypedSignalTracker<T>*>(it->second);
        return typed ? typed->prev_value() : T{};
    }
    
    template<typename T>
    T curr_value(T* sig) const {
        auto it = trackers_.find(static_cast<void*>(sig));
        if (it == trackers_.end()) return T{};
        auto* typed = dynamic_cast<TypedSignalTracker<T>*>(it->second);
        return typed ? typed->curr_value() : T{};
    }
};

class TestbenchScheduler {
public:
    using Action = std::function<void()>;
    
private:
    // Time-based events
    struct TimeEvent {
        uint64_t time;
        Action action;
        
        bool operator>(const TimeEvent& other) const {
            return time > other.time;  // Min-heap
        }
    };
    
    std::priority_queue<TimeEvent, std::vector<TimeEvent>, std::greater<>> time_events_;
    uint64_t current_time_{0};
    
    // Signal-based events
    struct SignalWatcher {
        std::vector<void*> watched_signals;
        std::function<void(const Signals&)> callback;
    };
    
    std::unordered_map<void*, std::unique_ptr<SignalTracker>> all_trackers_;
    std::vector<SignalWatcher> signal_watchers_;
    
public:
    // ===== Time-based scheduling =====
    
    void schedule_at(uint64_t time, Action action) {
        time_events_.push({time, std::move(action)});
    }
    
    void schedule_after(uint64_t delay, Action action) {
        schedule_at(current_time_ + delay, std::move(action));
    }
    
    uint64_t next_time_event() const {
        if (time_events_.empty()) return UINT64_MAX;
        return time_events_.top().time;
    }
    
    bool has_time_events() const {
        return !time_events_.empty();
    }
    
    // Execute all events scheduled for exactly this time
    void execute_time_events(uint64_t time) {
        current_time_ = time;
        while (!time_events_.empty() && time_events_.top().time == time) {
            auto event = std::move(time_events_.top());
            time_events_.pop();
            event.action();
        }
    }
    // Helper to deduce lambda type
    template<typename... SigPtrs>
    void always(std::function<void(const Signals&)> callback, SigPtrs... signals) {
        SignalWatcher watcher;
        watcher.callback = std::move(callback);
        
        // Register each signal
        (register_signal(signals, watcher), ...);
        
        signal_watchers_.push_back(std::move(watcher));
    }



    
    // Sample all tracked signals (call after eval)
    void sample_signals() {
        for (auto& [ptr, tracker] : all_trackers_) {
            tracker->sample();
        }
    }
    
    // Check watchers and fire callbacks with snapshot
    bool check_signal_watchers() {
        Signals events(all_trackers_);
        
        bool any_fired = false;
        for (const auto& watcher : signal_watchers_) {
            bool should_fire = false;
            for (void* sig_ptr : watcher.watched_signals) {
                if (all_trackers_[sig_ptr]->changed()) {
                    should_fire = true;
                    break;
                }
            }
            
            if (should_fire) {
                watcher.callback(events);
                any_fired = true;
            }
        }
        
        return any_fired;
    }
    
    // ===== Utility =====
    
    bool has_events() const {
        return !time_events_.empty() || !signal_watchers_.empty();
    }
    
    uint64_t current_time() const { 
        return current_time_; 
    }
    
private:
    template<typename T>
    void register_signal(T* sig, SignalWatcher& watcher) {
        void* ptr = static_cast<void*>(sig);
        watcher.watched_signals.push_back(ptr);
        
        // Create tracker if new
        if (all_trackers_.find(ptr) == all_trackers_.end()) {
            all_trackers_[ptr] = std::make_unique<TypedSignalTracker<T>>(sig);
        }
    }

};