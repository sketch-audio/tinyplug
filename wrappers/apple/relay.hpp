#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <utility> // std::move

#include <dispatch/dispatch.h>
#include <pthread.h> // pthread_main_np

namespace tiny {

class Relay {
public:

    using Execute = std::function<void()>;

    struct Spec {
        Execute execute{[](){}};
        double interval{0.1}; // Seconds.
    };

    explicit Relay(Spec spec) { this->_start(std::move(spec)); }
    ~Relay() { this->_stop(); }

    // No copy, no move.
    Relay(const Relay&) = delete;
    auto operator=(const Relay&) -> Relay& = delete;
    Relay(Relay&&) = delete;
    auto operator=(Relay&&) -> Relay& = delete;

    auto post() -> void
    {
        _state->posted.store(true, std::memory_order_release);
    }

private:

    struct State {
        std::atomic<bool> alive{true};
        std::atomic<bool> posted{false};
        Execute execute{};
    };

    // We have to share with GCD.
    std::shared_ptr<State> _state{std::make_shared<State>()};
    dispatch_source_t _source{};

    auto _start(Spec spec) -> void
    {
        if (!spec.execute) return;

        _state->execute = std::move(spec.execute);

        auto queue = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        _source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        if (_source == nullptr) return;

        const auto now = dispatch_time(DISPATCH_TIME_NOW, 0);
        const auto interval_ns = static_cast<uint64_t>(spec.interval * NSEC_PER_SEC);
        dispatch_source_set_timer(_source, now, interval_ns, interval_ns / 2);

        // Block owns state too.
        const auto state = _state;
        dispatch_source_set_event_handler(_source, ^{
            if (!state->alive.load(std::memory_order_acquire)) return;

            // Handle the post.
            if (!state->posted.exchange(false, std::memory_order_acq_rel)) return;

            dispatch_async(dispatch_get_main_queue(), ^{
                if (!state->alive.load(std::memory_order_acquire)) return;
                state->execute(); // Small race window here... OK as long as destructor runs on main.
            });
        });

        dispatch_resume(_source);
    }

    auto _stop() -> void
    {
        assert(pthread_main_np() > 0 && "Relay must be destroyed on the main thread!");
        _state->alive.store(false, std::memory_order_release);

        if (_source == nullptr) return;
        dispatch_source_cancel(_source);
#if !OS_OBJECT_USE_OBJC
        dispatch_release(_source);
#endif
        _source = nullptr;
    }
    
};

} // namespace tiny
