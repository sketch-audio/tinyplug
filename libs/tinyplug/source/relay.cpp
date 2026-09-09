#include <tinyplug/relay.hpp>

#include <algorithm>
#include <cstdint>
#include <utility> // std::move

#include <tinyplug/platform_defs.hpp>
#include <tinyplug/tiny_log.hpp>

#if TINY_PLATFORM_APPLE
    #include <dispatch/dispatch.h>
    #if TINY_LOG_ENABLED
        #include <pthread.h> // pthread_main_np, for the off-main diagnostic only.
    #endif
#elif TINY_PLATFORM_WINDOWS
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

namespace tiny {

Relay::Relay(Spec spec) { this->_start(std::move(spec)); }
Relay::~Relay() { this->_stop(); }

#if TINY_PLATFORM_APPLE

// This TU is plain C++, so OS_OBJECT_USE_OBJC is 0 here and stays 0: dispatch objects are
// not ARC-managed and `dispatch_release` is the correct pairing, unconditionally. That
// certainty is the reason this lives in a .cpp rather than the header.
static_assert(!OS_OBJECT_USE_OBJC, "relay.cpp must be compiled as plain C++, not ObjC++.");

auto Relay::_start(Spec spec) -> void
{
    if (!spec.execute) return;

    _state->execute = std::move(spec.execute);

    auto queue = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    auto source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    if (source == nullptr) return;

    const auto now = dispatch_time(DISPATCH_TIME_NOW, 0);
    const auto interval_ns = static_cast<uint64_t>(spec.interval * NSEC_PER_SEC);
    dispatch_source_set_timer(source, now, interval_ns, interval_ns / 2); // Leeway coalesces.

    // Block owns state too.
    const auto state = _state;
    dispatch_source_set_event_handler(source, ^{
        if (!state->alive.load(std::memory_order_acquire)) return;

        // Handle the post. An idle relay never reaches `execute`, so it never touches the
        // owner — the dangerous window is a posted proposal, not the whole time the timer
        // runs. Don't "simplify" this check away.
        if (!state->posted.exchange(false, std::memory_order_acq_rel)) return;

        dispatch_async(dispatch_get_main_queue(), ^{
            if (!state->alive.load(std::memory_order_acquire)) return;
            // Race window: a stop running off the main thread can overlap this call, so
            // `execute` must tolerate an owner being torn down under it. See `_stop`.
            state->execute();
        });
    });

    _timer = source;
    dispatch_resume(source);
}

auto Relay::_stop() -> void
{
    if (_timer == nullptr) return; // Idempotent: never started, or already stopped.

    // First and unconditionally, so the diagnostic below can never leave us in a worse
    // state than doing nothing: any handler not yet started observes this and bails.
    _state->alive.store(false, std::memory_order_release);

    // Stopping on main makes the `alive` check airtight, because the main queue is serial:
    // a stop running on it cannot interleave with a block already executing there. Off main
    // that argument is gone and a delivery already inside `execute` can run to completion
    // alongside us — so the callable must tolerate it. Ableton Live calls AUv3's
    // `-deallocateRenderResources` off main, so this is a real case, not a host bug; it is
    // logged rather than asserted because aborting the host over it would be far worse than
    // the race, and because the clients that can reach it are self-guarding (AUv3's weak
    // self, and an owner still alive at every other reachable stop point).
#if TINY_LOG_ENABLED
    if (pthread_main_np() == 0) {
        TINY_LOG_WARN(lifecycle, "Relay stopped off the main thread; a delivery may overlap.");
    }
#endif

    auto source = static_cast<dispatch_source_t>(_timer);
    dispatch_source_cancel(source); // Async: never waits, so this cannot hang the host.
    dispatch_release(source);
    _timer = nullptr;
}

#elif TINY_PLATFORM_WINDOWS

namespace {

// Runs on a pool thread, with no hop — the teardown barrier below is a real rundown, so
// the callback cannot outlive the owner and needs no serial context. Clients get a pool
// thread rather than the main thread here; that is the same deal VST3's
// Outbound_message_shuttle already makes for worker traffic.
auto CALLBACK relay_callback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) -> VOID
{
    auto* state = static_cast<Relay::State*>(context);

    // Bail before touching `execute` so an idle relay never reaches the owner.
    if (!state->posted.exchange(false, std::memory_order_acq_rel)) return;
    if (!state->alive.load(std::memory_order_acquire)) return;

    state->execute();
}

} // namespace

auto Relay::_start(Spec spec) -> void
{
    if (!spec.execute) return;

    _state->execute = std::move(spec.execute);

    // The state outlives the timer: `_stop` joins before the shared_ptr can drop.
    auto timer = CreateThreadpoolTimer(&relay_callback, _state.get(), nullptr);
    if (timer == nullptr) return;

    // Clamp to 1ms: a period of 0 makes SetThreadpoolTimer one-shot.
    const auto interval_ms = std::max<DWORD>(1, static_cast<DWORD>(spec.interval * 1000.0));

    // Negative 100ns units = relative due time.
    auto due = ULARGE_INTEGER{};
    due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(spec.interval * 10'000'000.0));

    auto due_time = FILETIME{.dwLowDateTime = due.LowPart, .dwHighDateTime = due.HighPart};

    _timer = timer;
    SetThreadpoolTimer(timer, &due_time, interval_ms, interval_ms / 2); // Window length coalesces.
}

auto Relay::_stop() -> void
{
    if (_timer == nullptr) return; // Idempotent: never started, or already stopped.

    _state->alive.store(false, std::memory_order_release);

    // Order is load-bearing. Never reach this from a callback (self-deadlock) or from
    // DllMain / a static destructor (the pool can need the loader lock).
    auto timer = static_cast<PTP_TIMER>(_timer);
    SetThreadpoolTimer(timer, nullptr, 0, 0);     // No further firings.
    WaitForThreadpoolTimerCallbacks(timer, TRUE); // Cancel pending AND join in-flight.
    CloseThreadpoolTimer(timer);
    _timer = nullptr;
}

#endif

} // namespace tiny
