#pragma once

#include <atomic>
#include <functional>
#include <memory>

namespace tiny {

// A coalescing deferral timer: `post()` is realtime-safe (one store) and the callback
// runs later, off the audio thread, at most once per interval no matter how many posts
// landed. Both backends use an OS-provided shared pool, so an instance costs no thread.
//
// The platform timer is type-erased so no consumer sees <dispatch/dispatch.h> or
// <windows.h>. That is not just hygiene: OS_OBJECT_USE_OBJC is 0 in a C++ TU and 1 in an
// ObjC++ one, and this header reaches both in the same target (AUv2's effect.hpp is
// included by effect.cpp and view_factory.mm), so an inline teardown would be two
// different function bodies under one weak symbol — an ODR violation whose symptom is a
// leaked dispatch source. Keeping it in relay.cpp makes the ownership rule one answer.
class Relay {
public:

    using Execute = std::function<void()>;

    struct Spec {
        Execute execute{[](){}};
        double interval{0.1}; // Seconds.
    };

    explicit Relay(Spec spec);
    ~Relay();

    // No copy, no move.
    Relay(const Relay&) = delete;
    auto operator=(const Relay&) -> Relay& = delete;
    Relay(Relay&&) = delete;
    auto operator=(Relay&&) -> Relay& = delete;

    // The audio-thread entry point, so it stays inline: one release store, no allocation,
    // no lock. Coalescing is the point — N posts inside one interval cost one callback.
    auto post() -> void
    {
        _state->posted.store(true, std::memory_order_release);
    }

    // The block shared with the OS timer. Public only so relay.cpp's platform callback can
    // name it — not part of the API, and you cannot get one from a Relay. No platform types
    // in it, which is why it can stay in the header.
    struct State {
        std::atomic<bool> alive{true};
        std::atomic<bool> posted{false};
        Execute execute{};
    };

private:

    // We have to share with the OS timer.
    std::shared_ptr<State> _state{std::make_shared<State>()};

    // dispatch_source_t on Apple, PTP_TIMER on Windows — both pointers. Erased rather
    // than pimpl'd behind a second allocation: it is the only platform-typed member.
    void* _timer{nullptr};

    auto _start(Spec spec) -> void;
    auto _stop() -> void;

};

} // namespace tiny
