# VST3 latency relay

Status: **implemented, untested in a host.** Parts 1-3 have landed, plus the optional
AUv2 rescoping. Diagnosis was corroborated against the VST3 SDK, the CLAP spec, and the
four other wrappers. This closes a shipping bug that truncates Ableton Live bounces.

What landed, against the plan below:

- `tiny::Relay` moved to `libs/tinyplug/include/tinyplug/relay.hpp` (core, not
  `tiny_platform` — it is a processor-side scheduling primitive and its siblings
  `Task_manager` / `Serial_queue` all live in core; `tiny_log` is the precedent for a
  platform-conditional core file). Win32 `CreateThreadpoolTimer` backend added; Apple
  backend logic unchanged; `_stop` is now idempotent. No forwarding header — the four
  `../apple` cmake references were updated and `wrappers/apple/` is gone.

  The implementation went into a new `libs/tinyplug/source/relay.cpp`, leaving the header
  free of `<dispatch/dispatch.h>` and `<windows.h>`; only the timer handle is type-erased
  (`void*`), so `State` and the audio-thread `post()` stay inline and no second allocation
  appears. That also **fixed a live ODR violation** in the old header: it reached both a
  C++ and an ObjC++ TU in the same target (AUv2's `effect.hpp` via `effect.cpp` and
  `view_factory.mm`), and `OS_OBJECT_USE_OBJC` is 0 in the former and 1 in the latter, so
  `Relay::_stop` was emitted as two different weak bodies under one symbol — one calling
  `dispatch_release`, one not. Verified by compiling the old header both ways. AUv2 has no
  `-fobjc-arc`, so whenever the linker picked the `.mm` body the dispatch source leaked on
  every teardown. A `static_assert(!OS_OBJECT_USE_OBJC)` in relay.cpp now pins the answer.
  The missing `NOMINMAX` (which every other `windows.h` site in the repo sets) went in too.
- **Part 1** in `setupProcessing`, `_needs_report` deleted. Also sends on the
  `setActive(true)` pending branch.
- **Part 2** relay scoped to `setActive(true)`/`setActive(false)`, stopped again from
  `terminate()` and the destructor. `_router`/`_to_ctrl` hoisted out of the worker `#if`
  on both sides, `notify()` unconditional, `tiny/latency/changed` routed to
  `restartComponent(kLatencyChanged)`. The `latency_param_id` output-parameter tunnel is
  gone; the parameter itself stays declared.
- **Part 3** offline gate on the propose path in VST3, CLAP, AUv2 and AUv3. VST3 now also
  reads `processSetup.processMode`. AAX unchanged, with the assumption recorded at
  `alg_proc.cpp`.
- **Both AUs rescoped and normalized**, which departs from the plan's "leave AUv3 alone".
  AUv2: relay emplaced in `Initialize`, reset in `Cleanup`, destructor as backstop (no
  `PreDestructor` override — redundant, since `Cleanup` covers the initialized case and an
  uninitialized AU never emplaced the relay). AUv3: started in
  `-allocateRenderResourcesAndReturnError:`, stopped in `-deallocateRenderResources`, with
  `-dealloc` as the backstop.

  The plan's reason for keeping AUv3 at object lifetime — "a proposal made in the last
  block before `deallocateRenderResources` still reaches the host" — does not hold.
  `deallocateRenderResources` calls `_kernel.deInitialize()`, which clears
  `_pending_latency` on that same call; when the relay next fires (up to 100ms later)
  `peek_latency_secs()` falls back to `_latency`, `updateReportedLatency` compares equal
  and early-returns. The wider scope delivers nothing. It was also a race, not a
  guarantee, and a notification that did land would describe a configuration the host had
  already torn down. The `__weak` self argument is orthogonal — that is callback lifetime
  safety, not scope, and it survives narrowing untouched.

  The one asymmetry that is real runs opposite to the plan's: `-dealloc` is a *worse* stop
  point than `-deallocateRenderResources`, because `Relay::_stop` asserts main thread and a
  final `release` can arrive on any thread. AUAudioUnit also makes no `AUBase`-style
  guarantee that `deallocateRenderResources` precedes `dealloc`, which is why AUv3 keeps
  its backstop where AUv2 could drop the `PreDestructor` one.
- **Not** taken: the optional COM `addRef` hardening, and everything under
  "Deferred / related".

One empirical finding already, from a first AUv3 run: **Ableton Live calls
`-deallocateRenderResources` off the main thread.** The Apple `Relay::_stop` asserted
`pthread_main_np()`, so the rescoping above turned that into an abort in Debug builds. The
assert is now a `TINY_LOG_WARN` — aborting the host over it is far worse than the race, and
the race itself is tolerable at every reachable stop point (AUv3's `__weak` self yields nil
during `dealloc` and a real retain otherwise; AUv2 and VST3 still have a live owner at
`Cleanup` / `setActive(false)` / `terminate`, and their destructor paths never reach `_stop`
because the earlier door already disengaged the relay). The `alive` store now happens before
the diagnostic, so a stop can never leave things worse than doing nothing. Scope was kept as
normalized: `-dealloc` has no thread guarantee either and is the worse of the two points.

Note this also means the wide-scope-is-safer argument has a real limit: the timer must not
outlive the owner's ability to service it, and no AU entry point gives us a documented thread.

Nothing else here has run in a host yet — the test plan at the bottom is untouched.

## The bug

In Ableton Live, bouncing at an export sample rate **different from the session rate**
cuts the plug-in's audio at an indeterminate point part way through the render. Same-rate
bounces are clean. JUCE and CPLUG plug-ins in the same session are unaffected.

Chain:

1. Live switches the plug-in to offline (and, for a differing export rate, to the export
   rate). Either way it calls `setupProcessing` — it must, because `processMode` lives in
   `ProcessSetup`.
2. [`audio_effect.cpp:143-146`](../wrappers/vst3/source/audio_effect.cpp) recomputes
   `_latency` and sets `_needs_report`, but deliberately does **not** sync
   `_reported_latency`:
   ```cpp
   _latency = _processor->latency_samps();
   _needs_report.store(true, ...); // Defer the normal-path latency notification to `process`.
   ```
3. On the **first block of the offline render**, `audio_effect.cpp:610-618` sees
   `_latency != _reported_latency` and calls `notify()`, which writes an output parameter
   change on `latency_param_id` (`audio_effect.cpp:594-598`).
4. Live forwards it to the controller. [`controller.cpp:651-656`](../wrappers/vst3/source/controller.cpp)
   calls `restartComponent(kLatencyChanged)` — on the message thread, whenever Live gets
   round to it. An offline render runs many times faster than realtime, so "whenever" is
   seconds into the exported audio. **That is the indeterminate cut point.**

Two harm mechanisms, one root cause:

- **Live mis-ingests `outputParameterChanges` during an offline bounce.** We already know
  this — `audio_effect.cpp:581-586` gates meters on exactly that, quoting "Live can crash
  ingesting output-parameter meters during an offline bounce." The latency notify is the
  **one remaining ungated output-param emission**, and it is reachable only on a rate-changed
  bounce. That is why the meter fix looked complete: it was verified on same-rate bounces.
- **`kLatencyChanged` mid-render.** `ivstaudioprocessor.h:317-326` is explicit: *"If during
  the use of the plug-in this latency change, the plug-in has to inform the host by using
  `IComponentHandler::restartComponent (kLatencyChanged)`, this could lead to audio playback
  interruption because the host has to recompute its internal mixer delay compensation."*

### Why a rate change always trips it

Latency in samples moves at every rate for these products:

- Fixed-millisecond latency: `sketch_dsp::Skew::_latency_ms = 0.67f` (Doctor Vibe, Lil Doc)
  → `ceil(0.67ms x sr)` = **30 / 33 / 60 / 65 / 119 / 129** samples at
  44.1 / 48 / 88.2 / 96 / 176.4 / 192k.
- Any product in HQ mode: `resolve_oversample_factor` steps 4 → 2 → 1 across the 48k / 96k
  boundaries, *and* `spk::Halfband_fir::reset` designs its kernel from `20000/(sr/2)`, so
  `nc` — and the group delay in samples — differs at 44.1k vs 48k as well.

At the same rate `_latency == _reported_latency`, so `notify()` never fires. The trigger is
exactly the reported condition.

### Why VST3 only

VST3 is the only format that reports a configure-time latency change **from inside
`process`**. Every other wrapper reports it synchronously at configure time, on the thread
the host called us on:

| format | configure-time | process-time | who owns the deferral thread |
|---|---|---|---|
| AUv2 | `PropertyChanged` direct in `Initialize` (`effect.cpp:108-111`) | `_relay->post()` (`effect.cpp:1311-1314`) | shared GCD pool |
| AUv3 | `[self updateReportedLatency]` direct in `allocateRenderResources` (`audio_unit.mm:556-560`) | `_relay->post()` (`DSPKernel.hpp:263-271`) | shared GCD pool |
| CLAP | `latency_ext->changed` in `activate` (`plugin.cpp:72-77`) | `host->request_restart` (`plugin.cpp:298-305`) | nobody — spec-sanctioned from audio thread |
| AAX | ring push from `alg_init` (`alg_proc.cpp:180-189`) | ring push from render (`alg_proc.cpp:403-421`) | the host's Direct Data timer |
| **VST3** | **deferred into `process`** | output param from `process` | none — hence the bug |

**Every other format already has the configure-direct / relay-for-process split this plan
adds to VST3.** VST3 lacks it because the processor is a separate COM component with no
`IComponentHandler`, so it invented an output-parameter tunnel and used it for both paths.

JUCE is immune because its VST3 plug-ins are single-component: processor and controller are
the same object, so `prepareToPlay` can call `restartComponent` directly on the message
thread, before the render starts.

## Design

### The two paths are structurally different

**Path 1 — configure-time.** Originates in `setupProcessing`, which is `[UI-thread &
Initialized]`. `IConnectionPoint::connect` is `[UI-thread & Initialized]` too, so the peer
connection already exists. **No render is in flight.** We can call `sendMessage` inline —
no queue, no relay, no timer, no thread. *This is the path that causes the bug.*

**Path 2 — runtime proposal.** `context.propose_latency`, raised on the audio thread by a
quality-mode switch. Needs deferral. But it is latency-tolerant by design: per
`shared/processor/effect_adapter.hpp:170-183` in `all_plugins`, the quality swap is now
**pre-emptive and does not wait for the ack**. A late or dropped notification costs only
delay-compensation accuracy and the `quality_actual` ghosting, and `getLatencySamples()`
peeking `_pending_latency` still completes the handshake whenever the host asks.

That asymmetry is what keeps the relay narrow.

### Scope answer

> The three relay formats are the AUs and VST3; they need the relay only for latency
> changes raised during `process`, not for configure, so it can be scoped narrower than
> instance lifetime.

Correct, with one exception:

- **VST3** — yes. Scope to `setActive(true)` / `setActive(false)`. New.
- **AUv2** — yes, and it *already* has the configure-direct/relay-for-process split; only
  the scoping is loose (constructor/destructor, `effect.cpp:58` and `effect.cpp:68`).
  Narrowing to `Initialize`/`Cleanup` is an optional improvement, not a fix.
- **AUv3** — **no, leave it.** It deliberately spans the AU lifetime, and says why at
  `audio_unit.mm:123-126`: *"Scoped to the AU, not to render resources, so a proposal made
  in the last block before `deallocateRenderResources` still reaches the host."* It is also
  the only one that is genuinely race-free, via an ObjC `__weak` self — the weak→strong load
  is atomic against deallocation and yields a real retain, so the AU cannot die under the
  callback. Narrowing it would trade a documented benefit for nothing.

### Why not a polling thread

Co-opting `Outbound_message_shuttle` (`messaging.hpp:69-109`) would give every VST3 instance
a `std::thread` sleeping at 16ms — 62.5 wakeups/s/instance, ~1250/s across 20 instances, to
service an event that fires perhaps twice a session. It would also be the only busy timer in
a non-worker plug-in; the two threads a plug-in already owns (`Task_launcher` and
`Serial_queue` inside the controller's `Task_manager`) are condition-variable-blocked and
cost nothing idle.

`tiny::Relay` is the right shape and already exists: a GCD `dispatch_source` timer on the
**shared global utility queue** with `leeway = interval/2` so the OS coalesces it. No thread
is created per instance. AUv2 and AUv3 already pay zero threads for this.

## Part 1 — configure-time goes direct (fixes the bug)

`wrappers/vst3/source/audio_effect.cpp`, in `setupProcessing`, replacing lines 143-146:

```cpp
_latency = _processor->latency_samps();

// The host re-queries getLatencySamples() after setup ([UI-thread & Setup Done]), so a
// reconfigure is not a change to announce — but a host that does not re-query still needs
// telling, and this is the one place we can tell it with no render in flight.
if (_latency != _reported_latency.exchange(_latency, std::memory_order_relaxed)) {
    _to_ctrl.send_pod(k_latency_changed_id, _latency.load(std::memory_order_relaxed));
}
```

The `exchange` syncs the shadow **whether or not the send lands**, so a reconfigure can never
again be mistaken for a mid-render change. That alone kills the bug even if `sendMessage`
fails. `_needs_report` is deleted.

In `setActive(true)`, the pending-proposal branch (lines 174-179) already consumes
`_pending_latency`; have it send as well rather than relying on the host having noticed the
parameter.

### Belt

`getLatencySamples()` (`audio_effect.cpp:759-768`) already returns the fresh value and
already peeks `_pending_latency`. Nothing about correctness may depend on the message
landing — a host that never connects a controller (offline scanning, render-only hosts)
gets `kResultFalse` from `sendMessage` and must still see the right latency.

## Part 2 — the relay for process-time proposals

### `tiny::Relay`, cross-platform

Move `wrappers/apple/relay.hpp` → `libs/tinyplug/include/tinyplug/relay.hpp`, keeping the
existing API verbatim so AUv2/AUv3 need no source change:

```cpp
struct Spec { Execute execute{[](){}}; double interval{0.1}; };
explicit Relay(Spec spec);
~Relay();
auto post() -> void;   // one relaxed/release store, audio-thread safe
```

Clients keep the `std::optional<Relay>` emplace/reset pattern AUv2 and AUv3 already use.

**Two backends, two different — individually sound — safety arguments.**

| | Apple (`dispatch_source`) | Windows (`CreateThreadpoolTimer`) |
|---|---|---|
| callback lands on | main queue (hop preserved) | pool thread, no hop |
| cancel semantics | async, joins nothing | **synchronous join** |
| teardown thread | must be the serial context the callback lands on | any |
| coalescing | `leeway = interval/2` | `SetThreadpoolTimer` window length |
| residual hazard | flag-vs-destructor race (existing) | deadlock if callback is blocked in host code |

**Apple: unchanged.** Keep the main-queue hop even for VST3. `sendMessage` from the main
thread is fine, and it means all three clients share one safety story. Keep
`assert(pthread_main_np() > 0)` at `relay.hpp:83` — it documents the invariant that makes
the `alive` check at `relay.hpp:73` sound: the main queue is serial, so a destructor running
on it cannot interleave with a block already executing there, and any block not yet started
observes `alive == false`.

**Windows teardown, in this order:**

```cpp
SetThreadpoolTimer(_timer, nullptr, 0, 0);        // no further firings
WaitForThreadpoolTimerCallbacks(_timer, TRUE);    // cancel pending AND join in-flight
CloseThreadpoolTimer(_timer);
```

`WaitForThreadpoolTimerCallbacks(..., TRUE)` is a real rundown barrier — when it returns, no
callback is running and none will start. That is strictly stronger than GCD, and it means the
Windows backend needs no main-thread requirement at all.

Windows rules, all load-bearing:

- **Never call `WaitForThreadpoolTimerCallbacks` from inside the callback** (self-deadlock).
- **Never reach teardown from `DllMain` or a static destructor.** The pool callback can need
  the loader lock. Per-instance, host-driven teardown only — no global or static `Relay`.
- **The join can deadlock** if a callback is already inside the host's `notify` and the
  destroying thread is the one that host is waiting on. Scoping to `setActive` (below) is
  what keeps that window shut. Note the existing `Outbound_message_shuttle::stop()` already
  joins a `std::thread` from its destructor and carries the identical exposure today.
- If we ever grow to more than one timer per instance, switch to
  `CreateThreadpoolCleanupGroup` + `CloseThreadpoolCleanupGroupMembers` so teardown stays one
  barrier rather than N.

### Lifetime: scope to the active window, plus backstops

Start in `setActive(true)`, stop in `setActive(false)` — the hooks `_shuttle` already uses at
`audio_effect.cpp:180-188`.

Why this and not object lifetime:

- `setActive` is `[UI-thread & Setup Done]` **by spec**, so on Apple the "destructor runs on
  main" precondition stops being an assumption about host behaviour and becomes a guarantee.
- On Windows the join runs while the host is calling *into* us, so it is not simultaneously
  blocked waiting *on* us — which is the exact shape the deadlock needs. Nowhere near loader
  teardown.
- No timer exists at all while the plug-in is inactive, which is most instances most of the
  time.

**Stop from every teardown door, idempotently**, because a host can be destroyed without a
`setActive(false)`:

1. `setActive(false)` — normal path.
2. `terminate()` (`audio_effect.cpp:113-119`, currently a bare pass-through) — hosts skip
   this far less often than they skip `setActive(false)`.
3. `~Audio_effect()` — last resort.

`Relay::_stop()` needs an early-out **before** the assert so a second call is a no-op
(`Outbound_message_shuttle::stop()` is already idempotent via `_running.exchange`).

Dropping a proposal at deactivation is already covered: `setActive(true)` consumes
`_pending_latency` (`audio_effect.cpp:174-179`) and `getLatencySamples()` peeks it.

**Idle relays never touch the owner.** The Apple handler bails at
`if (!state->posted.exchange(false, ...)) return;` before dereferencing `execute`. Keep that
property in the Windows backend — do not "simplify" the `posted` check away. It means the
dangerous window is not "the relay is running" but "the few ms after a genuine proposal",
which is why this pattern has survived in AUv2/AUv3.

### Optional hardening

VST3 components are COM-refcounted (`DELEGATE_REFCOUNT`). The relay could `addRef` the owner
while started and release on stop. Then a host that skips all three teardown doors **leaks
one component** instead of causing a use-after-free or a hang. The reference cycle is
deliberate — it is what makes failing to stop survivable, not a replacement for stopping.
Not available in AUv2 (an `Effect` is placement-new'd into `mInstanceStorage`, there is
nothing to retain), which is exactly why the Apple relay ended up with flag-plus-assert.

## Part 3 — the offline gate (all five formats)

Every wrapper gates **meters** on offline and leaves the **latency proposal** ungated four
lines below. A bounce cannot usefully renegotiate delay compensation, and the pre-emptive
quality swap means nothing stalls if we stay quiet.

| format | meter gate | propose gate | file |
|---|---|---|---|
| VST3 | `audio_effect.cpp:581-586` | **missing** | `audio_effect.cpp:600-608` |
| CLAP | `plugin.cpp:292-296` | **missing** | `plugin.cpp:298-305` |
| AUv2 | `effect.cpp:1301-1306` | **missing** | `effect.cpp:1308-1315` |
| AUv3 | `DSPKernel.hpp:252-258` | **missing** | `DSPKernel.hpp:263-271` |
| AAX | `alg_proc.cpp:389-401` | `delay_comp` only | `alg_proc.cpp:403-421` |

Each is the same one-liner: fold the existing `offline` local into the propose condition.
CLAP's is the most worth taking alongside VST3 — `request_restart` makes the host deactivate
and reactivate mid-render; the spec sanctions it and says "the operation may be delayed by
the host", but the argument for staying quiet is identical.

**Also read `processSetup.processMode`.** `audio_effect.cpp:397` tests only
`data.processMode`; the canonical field is `ProcessSetup::processMode`, which
`AudioEffect` already stores as `processSetup`. Some hosts leave `ProcessData::processMode`
at 0. Testing both costs nothing and makes the meter gate un-defeatable too.

## File-by-file change list

### New

- `libs/tinyplug/include/tinyplug/relay.hpp` — moved from `wrappers/apple/relay.hpp`,
  gains the Win32 backend. Keep a forwarding header at the old path, or update the two
  Apple cmake entries (`make_auv2_plugin.cmake:34`, `make_auv3_plugin.cmake:142`) and the
  `target_include_directories(... ../apple)` lines.

### `wrappers/vst3/source/messaging.hpp`

- Add to the reserved-ID comment block (lines 22-28):
  `//   tiny/latency/changed      — processor → controller`
- Define `k_latency_changed_id` somewhere both sides see it (here, not in `adapters.hpp`,
  which is the parameter-space header).

### `wrappers/vst3/source/audio_effect.hpp`

- Lines 37-39: make `notify()` unconditional (drop the `#if TINY_HAS_WORKER`).
- Lines 167-168: hoist `_router` and `_to_ctrl` out of the worker `#if`. Leave `_shuttle`
  (171) and the worker queues inside it.
- Add `std::optional<Relay> _relay{};`.
- Delete `_needs_report` (126), `max_change_count` (139), `_change_count` (140).

### `wrappers/vst3/source/audio_effect.cpp`

- `setupProcessing` (121-166): Part 1 above.
- `setActive` (169-191): send on the pending-proposal branch; start/stop `_relay` beside the
  existing `_shuttle` calls.
- `terminate` (113-119): stop the relay.
- Destructor: stop the relay.
- `process`:
  - delete the `_needs_report` block (610-618) and the `add_output_event(latency_param_id, …)`
    inside `notify()` (594-598);
  - keep the propose branch (600-608) but replace `notify()` with `_relay->post()` and gate on
    `!is_offline_bounce`;
  - `is_offline_bounce` (397) also reads `processSetup.processMode`.
- Split `_setup_worker`'s router registration so the latency handler registers unconditionally.

### `wrappers/vst3/source/controller.hpp` / `.cpp`

- `controller.hpp:38-40`, `controller.cpp:52-56`: `notify()` unconditional.
- `controller.hpp:179-180`: hoist `_router` and `_to_proc` out of the worker `#if`.
- `controller.cpp:24-50`: split the unconditional router setup out of `_setup_worker`;
  register `tiny/latency/changed` → `restartComponent(kLatencyChanged)`.
- `controller.cpp:651-656`: delete the `latency_param_id` branch in `setParamNormalized`.

**Do not remove the parameter declaration at `controller.cpp:185-195`.** It is in the cached
parameter list of every saved session. It is read-only at `0x60000000`, far outside the
automatable range, so leaving it declared and simply never writing to it costs nothing.
Removing it is a saved-session compatibility decision that this plan does not need to take.

### `wrappers/vst3/make_vst3_plugin.cmake`

- Add the relay header to the source list (lines 17-26) and, if it stays under
  `wrappers/apple/`, the include directory. Windows backend needs no extra link libraries —
  the thread pool API is in `kernel32`.

### Other formats

- **CLAP** — `plugin.cpp:298-305`: gate `request_restart` on `!offline`. One line. No relay.
- **AUv2** — optional: move `_relay.emplace` from the constructor (`effect.cpp:58`) to
  `Initialize()` and `_relay.reset()` from the destructor (`effect.cpp:68`) to `Cleanup()`,
  plus a `PreDestructor()` override as backstop. The AudioUnit SDK **guarantees** `Cleanup()`
  runs before destruction if the AU was initialized — `AP_Close → DoPreDestructor →
  PreDestructor → PreDestructorInternal → DoCleanup → Cleanup` (`ComponentBase.cpp:60-73`,
  `AUBase.cpp:113-119`, `AUBase.cpp:217-228`) — so this hook cannot be skipped the way VST3's
  `setActive(false)` can. Caveat: `AudioUnitUninitialize` carries no documented thread
  guarantee, so this does **not** buy the spec-backed UI-thread promise the same move buys on
  VST3. Note also that `Cleanup()` already discards the pending proposal
  (`effect.cpp:117-123`), so scoping is consistent with a decision AUv2 has already made —
  and one AUv3 deliberately made the other way.
- **AUv3** — rescoped after all; see the status note at the top for why the "Scope answer"
  below does not hold up.
- **AAX** — no change. But record the assumption next to `alg_proc.cpp:180-189`: AAX is safe
  **because Pro Tools never changes the sample rate under a live instance** (session rate is
  fixed; Bounce to Disk ships a Conversion Quality control, so it renders at session rate and
  converts after), and because PT's response to `SetSignalLatency` is to adjust its own
  compensation rather than restart the component. It is **not** safe because delivery is
  synchronous — it is not. The ring entry waits for the next Direct Data wakeup, "roughly
  every 30 ms and not guaranteed to be regular" (`direct_data.hpp:22-23`), the same async hop
  VST3 has. If the trigger existed, the window would cost a 3-sample PDC offset at 44.1↔48,
  self-correcting at the next reset — not a cut.

  Note AAX **already implements Part 1**: `adopt_reset_state` seeds `reported_latency` from
  `runtime.accepted_latency` — what the host actually holds — so the comparison is silent on
  an ordinary reset and speaks up when the configuration genuinely moved
  (`alg_proc.cpp:109-113`, `alg_proc.cpp:145-152`). That is the design VST3 should have had.

## Test plan

1. **The repro.** Doctor Vibe in Live, session 44.1k, export 48k. Clean after Part 1 alone.
2. **Second independent trigger.** A product with zero SQ latency (Fuzz Droid, Hyper Boost,
   Scissor Hands) in **HQ** mode, same rate mismatch — exercises the oversample-factor step
   rather than the fixed-ms path. Should also be clean; should reproduce on the old build.
3. **Negative control.** Same product in **SQ** mode: latency is 0 at both rates, so it never
   reproduced and must stay clean.
4. **Path 2 still works.** Flip SQ↔HQ mid-playback in Live and Cubase; confirm the host
   renegotiates and `quality_actual` settles.
5. **Path 2 during a bounce.** Flip SQ↔HQ mid-export; confirm the new offline gate does not
   wedge the swap. It should not — the swap is pre-emptive.
6. **Load with nonzero latency** in Reaper and **FL Studio**. FL queries `getLatencySamples`
   and keeps processing without toggling `setActive` (see the wrapper notes in `CLAUDE.md`);
   the getter-as-acceptance semantics must survive intact.
7. **Windows teardown.** Instantiate/destroy under load with the editor open and closed;
   deactivate mid-proposal; unload the plug-in while active. Watch for hangs, not just crashes.
8. **Worker plug-in regression.** A `plug_worker.h` product still routes worker traffic after
   `_router`/`_to_ctrl` are hoisted out of the `#if`.
9. **No-controller host.** Confirm `sendMessage` failing degrades to the `getLatencySamples`
   belt rather than a wrong latency.

## Deferred / related

- **Merge the shuttle into the relay.** Once `Relay` is cross-platform,
  `Outbound_message_shuttle` becomes a `Relay` with a drain list, and worker plug-ins stop
  spending a polling thread too. That is the real "latency and worker share one shuttle"
  outcome — reached by deleting a thread rather than adding one.
- **AAX: PDC re-proposal.** `alg_proc.cpp:409-410` — *"PDC disabled by the host drops it
  outright — known unfixed bug: nothing re-proposes if it is switched back on."* Same class
  as this bug: a `reported_latency` shadow going stale against what the host holds. Worth
  fixing while the reasoning is loaded.
- **`tail_samps()` returns 0 everywhere.** `effect_adapter.hpp:448-451` and the
  `nugget_adapter` / `garden_adapter` equivalents in `all_plugins` all return `{}` ("Not yet
  a thing") → `kNoTail`. Hosts use it to decide how long to render past the last material, so
  reverb and delay tails are being truncated at the end of every export in every format.
  Unrelated to this bug; deserves its own ticket.
- **`_input_data` is sized to `maxSamplesPerBlock`** and `audio_effect.cpp:415` copies
  `data.numSamples` into it behind an `assert` only. A host handing an oversized block is a
  Release-mode heap overflow into the host's allocator. Not believed to be the Live bounce
  cause, but a Debug build bouncing at a differing rate would trip the assert at
  `audio_effect.cpp:414` and settle it in one run. Cheap to make defensive.
