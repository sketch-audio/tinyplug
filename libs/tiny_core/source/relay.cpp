#include <tiny_core/relay.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <utility> // std::move
#include <vector>

#include <tiny_core/platform_defs.hpp>

#if TINY_PLATFORM_APPLE
    #include <dispatch/dispatch.h>
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
    _state->repeating = spec.repeating;

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
        // runs. Don't "simplify" this check away; a repeating relay opts out knowingly.
        if (!state->posted.exchange(false, std::memory_order_acq_rel) && !state->repeating) return;

        dispatch_async(dispatch_get_main_queue(), ^{
            // Held for the whole call: a stop off main waits here rather than tearing the owner
            // down under us. Recursive, so an `execute` that stops its own relay cannot deadlock.
            const auto lock = std::lock_guard{state->delivering};
            if (!state->alive.load(std::memory_order_acquire)) return;
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

    // A delivery not yet started sees `alive` and bails. One already inside `execute` is
    // waited out: on main that cannot happen (the main queue is serial), and off main it is a
    // real case — Ableton Live calls AUv3's `-deallocateRenderResources` off main, and a host
    // may dispose of an AUv2 there — so after this returns nothing reaches the owner. It waits
    // only for a call main is already running, never for main itself, so it cannot hang on a
    // blocked main thread unless `execute` itself waits on the stopping thread.
    { const auto lock = std::lock_guard{_state->delivering}; }

    auto source = static_cast<dispatch_source_t>(_timer);
    dispatch_source_cancel(source); // Async: never waits, so this cannot hang the host.
    dispatch_release(source);
    _timer = nullptr;
}

#elif TINY_PLATFORM_WINDOWS

// Windows has no main dispatch queue, so this builds one: a hidden message-only window. A message
// sent to a window is handled on the thread that created it, and the first relay is constructed on
// the host's UI thread (VST3 `initialize` and `setActive` are [UI-thread] by spec), so its window
// procedure is the Windows equivalent of dispatch_get_main_queue(). Window messages, unlike thread
// messages, survive the modal loops of message boxes and host dialogs.
//
// One window and one pool timer per plug-in DLL, however many relays: this TU is compiled into each
// plug-in, and a window class registered with the DLL's own HINSTANCE is private to that module, so
// two plug-ins' dispatchers can't meet. The timer polls, because the audio thread can't PostMessage
// (a system call that can take locks); it runs at the shortest registered interval, and when any
// relay is due it posts ONE message. The window procedure then runs each due relay's `execute`.

namespace {

constexpr auto k_deliver = WM_APP + 1; // Run whatever is due.
constexpr auto k_destroy = WM_APP + 2; // Last relay stopped off the window's thread: destroy, then unpin. lparam: HMODULE.

auto this_module() -> HMODULE
{
    auto module = HMODULE{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&this_module), &module);
    return module;
}

// Adds a reference, so the DLL stays mapped while a message to its window is still queued.
auto pin_module() -> HMODULE
{
    auto module = HMODULE{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&this_module), &module);
    return module;
}

// Drops that reference without our own code ever returning into an unmapped image: the pool
// releases it after this callback has returned. Deferred, so the window procedure that scheduled it
// has long returned too.
auto CALLBACK unpin_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer) -> VOID
{
    CloseThreadpoolTimer(timer); // Allowed from its own callback: freed once the callback returns.
    FreeLibraryWhenCallbackReturns(instance, static_cast<HMODULE>(context));
}

auto unpin_later(HMODULE module) -> void
{
    auto* timer = CreateThreadpoolTimer(&unpin_callback, module, nullptr);
    if (timer == nullptr) return; // Leaks a reference: the DLL stays loaded, which is safe.
    auto due = ULARGE_INTEGER{};
    due.QuadPart = static_cast<ULONGLONG>(-100 * 10'000LL); // 100 ms, relative.
    auto due_time = FILETIME{.dwLowDateTime = due.LowPart, .dwHighDateTime = due.HighPart};
    SetThreadpoolTimer(timer, &due_time, 0, 0);
}

class Main_dispatcher {
public:

    // Never destroyed: a static destructor runs under the loader lock, where the pool can't be
    // waited on. The window and timer are gone by then anyway, once the last relay stopped.
    static auto instance() -> Main_dispatcher&
    {
        static auto* dispatcher = new Main_dispatcher{};
        return *dispatcher;
    }

    auto add(std::shared_ptr<Relay::State> state, double interval) -> void
    {
        const auto lock = std::lock_guard{_mutex};
        // A window dies with the thread that created it. If a host built the first relay on a
        // short-lived thread, every relay would go silent once it exited; rebuild on this one.
        if (_window != nullptr && !IsWindow(_window)) _window = nullptr;
        if (_window == nullptr && !_create_window()) {
            assert(false && "Relay: no message window, so no relay in this DLL will fire.");
            return;
        }
        if (_timer == nullptr) _timer = CreateThreadpoolTimer(&Main_dispatcher::_timer_callback, this, nullptr);
        assert(_timer != nullptr && "Relay: no pool timer, so no relay in this DLL will fire.");
        _entries.push_back(Entry{std::move(state), _to_duration(interval), Clock::now()});
        _retime();
    }

    // After the relay has marked itself dead and waited out a delivery in flight.
    auto remove(const Relay::State* state) -> void
    {
        auto timer = PTP_TIMER{};
        auto window = HWND{};
        {
            const auto lock = std::lock_guard{_mutex};
            std::erase_if(_entries, [state](const Entry& e) { return e.state.get() == state; });
            if (!_entries.empty()) { _retime(); return; }
            timer = std::exchange(_timer, nullptr);
            window = std::exchange(_window, nullptr);
            _period_ms = 0;
        }
        // Outside the lock: the timer callback takes it.
        if (timer) {
            SetThreadpoolTimer(timer, nullptr, 0, 0);
            WaitForThreadpoolTimerCallbacks(timer, TRUE);
            CloseThreadpoolTimer(timer);
        }
        if (!window) return;
        // DestroyWindow only works on the window's own thread. Anywhere else, ask it to destroy
        // itself, and keep the DLL mapped until it has, or a host that unloads us first would
        // dispatch that message into unmapped code.
        if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) destroy(window);
        else if (const auto module = pin_module(); !PostMessageW(window, k_destroy, 0, reinterpret_cast<LPARAM>(module))) FreeLibrary(module);
    }

private:

    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::shared_ptr<Relay::State> state{};
        Clock::duration interval{};
        Clock::time_point last{};
    };

    std::mutex _mutex{};
    std::vector<Entry> _entries{};
    HWND _window{};
    PTP_TIMER _timer{};
    DWORD _period_ms{};
    std::atomic<bool> _message_queued{};

    static auto _to_duration(double seconds) -> Clock::duration
    {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(seconds, 0.001)));
    }

    static constexpr auto k_class = L"tinyplug_relay";

    // Under `_mutex`.
    auto _create_window() -> bool
    {
        const auto module = this_module();
        auto wc = WNDCLASSEXW{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Main_dispatcher::_window_proc;
        wc.hInstance = module;
        wc.lpszClassName = k_class;
        RegisterClassExW(&wc); // Fails harmlessly if still registered; the class is this module's.
        _window = CreateWindowExW(0, k_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
        if (_window == nullptr) return false;
        // A message still queued when the last window was destroyed was discarded with it, so it
        // will never clear this; left set, the new window would never be told anything is due.
        _message_queued.store(false, std::memory_order_release);
        return true;
    }

    // On the window's thread. Windows doesn't unregister a DLL's classes when it unloads; tidy up
    // here instead. It fails harmlessly while another window of the class exists (a replacement
    // created while this one's destruction was still queued).
    static auto destroy(HWND window) -> void
    {
        DestroyWindow(window);
        UnregisterClassW(k_class, this_module());
    }

    // Under `_mutex`. Posted or ticking, and its own interval has come round: half a period of
    // slack, so a relay at the timer's own interval isn't skipped for jitter.
    auto _due(const Entry& e, Clock::time_point now) const -> bool
    {
        if (!e.state->repeating && !e.state->posted.load(std::memory_order_acquire)) return false;
        return now - e.last + std::chrono::milliseconds{_period_ms / 2} >= e.interval;
    }

    // Under `_mutex`. The timer runs at the shortest interval; longer ones are skipped until due.
    auto _retime() -> void
    {
        auto shortest = Clock::duration::max();
        for (const auto& e : _entries) shortest = std::min(shortest, e.interval);
        const auto ms = std::max<DWORD>(1, static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(shortest).count()));
        if (ms == _period_ms || _timer == nullptr) return;
        _period_ms = ms;
        auto due = ULARGE_INTEGER{};
        due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(ms) * 10'000LL); // Relative, 100 ns units.
        auto due_time = FILETIME{.dwLowDateTime = due.LowPart, .dwHighDateTime = due.HighPart};
        SetThreadpoolTimer(_timer, &due_time, ms, ms / 2); // Window length coalesces.
    }

    // Pool thread. Posts at most one message however many relays are due or ticks pile up.
    static auto CALLBACK _timer_callback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) -> VOID
    {
        auto& self = *static_cast<Main_dispatcher*>(context);
        const auto lock = std::lock_guard{self._mutex};
        // Due, not just posted: a posted slow relay would otherwise post a message every fast tick.
        const auto now = Clock::now();
        const auto wanted = std::ranges::any_of(self._entries, [&](const Entry& e) { return self._due(e, now); });
        if (!wanted || self._window == nullptr) return;
        if (!self._message_queued.exchange(true, std::memory_order_acq_rel)) {
            if (!PostMessageW(self._window, k_deliver, 0, 0)) self._message_queued.store(false, std::memory_order_release);
        }
    }

    // The window's thread: main.
    auto _deliver() -> void
    {
        _message_queued.store(false, std::memory_order_release);

        // Collected under the lock, run outside it: `execute` may construct or stop relays.
        auto due = std::vector<std::shared_ptr<Relay::State>>{};
        {
            const auto lock = std::lock_guard{_mutex};
            const auto now = Clock::now();
            for (auto& e : _entries) {
                if (!_due(e, now)) continue;
                // Bail before touching `execute`, so an idle relay never reaches its owner.
                if (!e.state->posted.exchange(false, std::memory_order_acq_rel) && !e.state->repeating) continue;
                e.last = now;
                due.push_back(e.state);
            }
        }
        for (const auto& state : due) {
            // Held for the whole call: a stop off main waits here rather than tearing the owner down
            // under us. Recursive, so an `execute` that stops its own relay can't deadlock.
            const auto lock = std::lock_guard{state->delivering};
            if (!state->alive.load(std::memory_order_acquire)) continue;
            state->execute();
        }
    }

    static auto CALLBACK _window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
    {
        switch (message) {
            case k_deliver: instance()._deliver(); return 0;
            case k_destroy:
                destroy(window);
                unpin_later(reinterpret_cast<HMODULE>(lparam));
                return 0;
            default: return DefWindowProcW(window, message, wparam, lparam);
        }
    }
};

} // namespace

auto Relay::_start(Spec spec) -> void
{
    if (!spec.execute) return;

    _state->execute = std::move(spec.execute);
    _state->repeating = spec.repeating;

    Main_dispatcher::instance().add(_state, spec.interval);
    _timer = _state.get(); // Only marks "registered": the dispatcher owns the platform objects.
}

auto Relay::_stop() -> void
{
    if (_timer == nullptr) return; // Idempotent: never started, or already stopped.

    // The same order as on Apple: a delivery not yet started sees `alive` and bails; one already
    // inside `execute` is waited out. On the window's thread that can't happen (it's serial, and the
    // mutex is recursive for an `execute` that stops its own relay). Never from DllMain or a static
    // destructor: removing the last relay waits on the pool, which can need the loader lock.
    _state->alive.store(false, std::memory_order_release);
    { const auto lock = std::lock_guard{_state->delivering}; }

    Main_dispatcher::instance().remove(_state.get());
    _timer = nullptr;
}

#endif

} // namespace tiny
