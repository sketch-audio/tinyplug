# Plan: Relay with pluggable main-thread delivery

> Status: **design.** Written 2026-09-25, after the fake-host and sanitizer work. The next
> step is the Windows validation pass, which this plan is meant to unblock.
>
> `Relay` ([relay.hpp](../libs/tiny_core/include/tiny_core/relay.hpp)) promises "the callback runs
> later, off the audio thread". On Apple it also runs on **main**, and several clients depend on
> that. On Windows it runs on a **thread-pool thread**, which quietly breaks those clients. This
> plan gives `Relay` a delivery that is really the main/UI thread on every platform, and lets a
> format that has its own main-thread hook use that instead.

## Why

Clients that need main-thread delivery today:

- **VST3, every processor → controller message.** `IConnectionPoint::notify` is
  `[UI-thread & Connected]`, and a host's connection proxy may drop a send from any other thread.
  The latency, block, state-snapshot and worker relays in
  [audio_effect.cpp](../wrappers/vst3/source/audio_effect.cpp) and
  [controller.cpp](../wrappers/vst3/source/controller.cpp) all send from `execute`. On Windows
  that's a pool thread. This isn't a regression: the deleted `Outbound_message_shuttle` sent from
  its own thread too. But it's wrong, and `tests/hosts/vst3_host.cpp`'s `Strict_proxy` will flag
  every one of these sends once that host runs on Windows.
- **AUv2 and AUv3 state ticks** (`State_image`, `repeating = true`). They're Apple-only, so they're
  fine today; listed because they rely on the same guarantee.
- **AUv2 latency `PropertyChanged`** and **AUv3 KVO**, both Apple-only.

## Current shape

- **Apple:** a GCD timer on a utility queue polls `posted` (or fires every interval if
  `repeating`), then `dispatch_async`s to the main queue. `execute` runs holding
  `State::delivering`, a recursive mutex. `_stop` sets `alive = false`, then takes and releases
  `delivering`, so it waits out a delivery already running and never waits for main itself.
- **Windows:** a thread-pool timer (`CreateThreadpoolTimer`) polls `posted` and calls `execute`
  **on the pool thread**. `_stop` is a real rundown (`WaitForThreadpoolTimerCallbacks`). Callbacks
  can overlap if one runs longer than the interval; nothing on Windows uses `repeating` yet.

`post()` must stay one release store: it's called from the audio thread.

## Design

`Relay` keeps its API (`post`, `Spec{execute, interval, repeating}`) and gains a delivery
strategy, chosen per platform, or supplied by the wrapper:

| Delivery | Where | How `execute` reaches main |
|---|---|---|
| Main queue | Apple (today) | GCD timer → `dispatch_async(main)` |
| Message window | Windows | one pool timer → `PostMessage` → a per-DLL hidden window's window procedure |
| Host callback | CLAP, any platform | `host->request_callback()` → `plugin->on_main_thread()` |

### Windows: one message window per DLL

A message sent to a window is handled on the thread that created the window. A window created on
the host's UI thread is therefore the Windows equivalent of the main dispatch queue.

- **One window per plug-in DLL, not per relay.** It's created lazily by the first `Relay`
  constructed on the UI thread. VST3 `initialize` is specified `[UI-thread]` for both components,
  so it's a reliable first touch. Register the window class with the DLL's own `HINSTANCE`: such a
  class is private to that module, so two plug-ins can't collide (the problem `configure_mac_view`
  solves for Objective-C classes doesn't exist here). `HWND_MESSAGE` parent: invisible, and never
  enumerated.
- **One pool timer for all relays.** The audio thread still can't call `PostMessage` (a system
  call that can take locks), so polling stays. A single timer at the shortest registered interval
  scans each relay's `posted` flag and, if any is set, sends **one** `WM_APP` message. The window
  procedure then runs `execute` for every flagged relay, each at its own interval. That's one
  kernel timer and one window however many relays a plug-in has, and at most one queued message.
- **A registry, not raw pointers in messages.** Relays register a `shared_ptr<State>` under an id.
  The window procedure looks entries up under a lock, so a relay stopped after its message was
  posted is skipped. Keep `delivering` and the stop order from Apple: `alive = false`, then take
  `delivering`. Stopping on the UI thread is then trivially serial, and stopping off it waits out a
  delivery in flight.
- **Why a window, not a thread message.** `PostThreadMessage`, and `SetTimer` with no window, post
  to the thread's queue, and modal loops (message boxes, host dialogs) **drop** such messages.
  Messages to a window survive modal loops. This is why JUCE and most frameworks use a hidden
  message window.

### The hard part: teardown and DLL unload

Delivery is simple; the lifetime isn't.

- `DestroyWindow` must be called on the thread that created the window. If the last relay stops
  off the UI thread, the window has to be asked to destroy itself by message.
- If the host unloads the DLL (`FreeLibrary`) while a message is still queued, Windows calls a
  window procedure whose code is no longer mapped, and the host crashes. Options, most
  conservative first:
  1. Hold a module reference (`GetModuleHandleEx` without `GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT`)
     while the window exists; the window procedure destroys the window on its last-relay message,
     then releases the reference. This is the self-contained answer.
  2. Destroy it in a framework shutdown call that each wrapper makes on the UI thread (VST3
     `terminate`, CLAP `destroy`). It's simpler, but relies on every wrapper calling it.
- Do nothing from `DllMain`: it runs under the loader lock.
- Keep the Windows rule already noted in `relay.cpp`: never stop a relay from inside its own
  callback. With the window design that becomes "never from inside `execute`", which the recursive
  `delivering` already tolerates on Apple; decide whether Windows matches.

### CLAP: use the host

CLAP already provides the right primitive on every platform:

- `clap_host::request_callback` → the host calls `clap_plugin::on_main_thread` on the main thread.
- The optional `timer-support` extension (`CLAP_EXT_TIMER_SUPPORT`) gives host-driven main-thread
  timers, if a periodic tick is ever needed there.

So the CLAP wrapper can supply a `request_callback` delivery: `post()` sets the flag as now,
something off the audio thread calls `request_callback` (the host allows that from any thread),
and `on_main_thread` drains flagged relays. The CLAP wrapper then needs no platform timer at all.
Check that `request_callback` is realtime-safe enough to call straight from `post`; if it isn't,
keep a pool timer to make the call.

AAX needs nothing: `TimerWakeup` is its main-thread tick and Direct Data its transport. AUv2 and
AUv3 are Apple-only and keep the main queue.

## Shape of the change

- `Relay::Spec` gains an optional delivery hook, or a small `Delivery` interface the wrapper passes
  (CLAP). The default is the platform one. Keep the header free of platform types; see the ODR note
  at the top of `relay.hpp`.
- A per-DLL `Main_dispatcher` (Windows) in `tiny_core/source/`, owning the window, the pool timer
  and the registry. `Relay` becomes a registration with it.
- Unify `repeating` handling: with one dispatcher, a repeating relay is just a relay that is always
  treated as posted, so the Windows overlap question goes away (the window procedure is serial).

## Tests

- `tests/tasks_test.cpp` Relay cases are `#if TINY_PLATFORM_APPLE` today; they need a Windows
  `pump_main` (a `PeekMessage`/`DispatchMessage` loop) and then run as-is: idle never fires; posts
  coalesce and run on main; nothing after destruction; a stop off main waits out a delivery.
- Add: a delivery runs during a modal loop (a `MessageBox` on a timer); two DLLs' dispatchers don't
  interfere; unload with a message queued (load a test DLL, post, `FreeLibrary`) doesn't crash.
- The real proof is `vst3_host` on Windows: `Strict_proxy` should report zero off-UI-thread sends.

## Related

- The Windows validation pass (ASan presets without `/RTC1`, STL annotation mismatches with
  prebuilt Skia and AAX, porting the fake hosts) is the context this plan was written for.
  `vst3_host` is the host that proves this change.
- [unified-edit-model.md](unified-edit-model.md): removes the AUv2 and AUv3 `repeating` state
  ticks' polling, which leaves `repeating` with no Apple clients either.
