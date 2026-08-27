# Plan: the TSan layer

> Status: **design.** The first concrete slice of [test-suite.md](test-suite.md).
> That plan describes five tiers and defers ThreadSanitizer to its Phase 9
> ("scheduled job", one line). This plan inverts that: TSan lands **first**,
> because it needs less scaffolding than the conformance tiers, it is the only
> tool that can see the class of bug this framework is structurally exposed to
> (three threads, five wrappers, lock-free everywhere), and building it forces
> the `tests/` skeleton that every other tier also needs.
>
> Scope of this document: how TSan is wired, what it can and cannot be pointed
> at, and the concrete target list. The invariant catalogue stays in
> [test-suite.md](test-suite.md) §5.

---

## 1. The constraint that shapes everything: where TSan can run

The request was "run the examples under TSan in either a real or simulated host
environment." The real-host half is not achievable, and it is worth writing down
why once so it doesn't get re-litigated.

**TSan cannot be injected into a DAW.**

| Obstacle | Detail |
|---|---|
| Runtime must initialize first | The TSan runtime reserves fixed shadow-memory regions and installs interceptors before any thread or allocation happens. The supported deployment is linking `-fsanitize=thread` into the **main executable**. A plug-in dylib built with TSan and loaded late into an uninstrumented host typically fails at `__tsan_init` — the address space it needs is already mapped. |
| `DYLD_INSERT_LIBRARIES` is blocked | The ASan escape hatch of preloading the runtime into an uninstrumented binary does not apply: every shipping DAW on macOS (Logic, Live, Pro Tools, notarized Reaper) uses the hardened runtime with library validation, which refuses inserted libraries. |
| Signing | Pro Tools requires PACE-wrapped, signed AAX binaries. A sanitizer build is neither. |
| AUv3 is out-of-process | The extension is loaded by a system-managed host process we do not own the executable of. |
| Universal binaries | TSan builds one slice at a time; the demos build `arm64;x86_64`. |
| Uninstrumented code is invisible | Even if it loaded, the host, Skia and the prebuilt SDK archives carry no instrumentation. TSan still intercepts their **pthread** primitives, so mutex-based synchronization inside them is seen — but plain atomics are not, which produces both false negatives and (worse) false positives against code that synchronizes without a mutex. |

**Therefore: TSan runs against a host we compile.** Three rigs, in increasing
fidelity and decreasing convenience:

| Rig | What it links | Sees | Cost |
|---|---|---|---|
| **A. Direct** | the framework's own primitives, driven by threads the test spawns | queues, `Task_manager`, `Serial_queue`, `log::Ring`, `Byte_ring` | trivial, no SDK |
| **B. In-process simulated host** | the real wrapper class, instantiated in a TSan-built test executable, driven from real audio / UI / worker threads | everything above **plus** the wrapper's own cross-thread state — the VST3 shuttle, the CLAP `_from_flush` queue, the AAX Direct Data ring, latency handshake atomics | needs the wrapper to compile headless |
| **C. Bundle-loading headless host** | a TSan-built CLI host that `dlopen`s a **TSan-built bundle** | everything above **plus** entry points, static initialization order, factory/global state, the real two-component wiring | needs a bundle build with the sanitizer and a real host driver |

Both B and C are "simulated hosts" in the sense the request meant. The important
design decision is that **B and C share one driver** (§4) — C is a linkage mode,
not a second harness.

Rig A is the honest answer to "run the examples under TSan": the example
*processors* and *workers* are plain C++ classes that a TSan-built runner can
drive directly, today, with no wrapper and no bundle. That is where this lands
first (§5, `tiny_tsan_<example>`).

---

## 2. What the audit found: the thread topology under test

Enumerated from the current tree so the driver can be written against something
real rather than against the prose in [CLAUDE.md](../CLAUDE.md).

**Threads the framework creates:**

| Thread | Owner | File |
|---|---|---|
| Worker | `Worker_runner::start` — polls two SPSC queues at `Model::update_period` | [tiny_worker.hpp:242](../libs/tinyplug/include/tinyplug/tiny_worker.hpp) |
| Serial queue | one per `Task_manager`, spawned in the member initializer | [serial_queue.hpp:33](../libs/tinyplug/include/tinyplug/serial_queue.hpp) |
| Background pool | `Task_launcher`, `_num_threads{1}` | [task_launcher.hpp:38](../libs/tinyplug/include/tinyplug/task_launcher.hpp) |
| Log drain | one per process, started by the first `log::Probe` construction | [tiny_log.cpp:372](../libs/tinyplug/source/tiny_log.cpp) |
| VST3 outbound shuttle | `Outbound_message_shuttle::start`, from `setActive(true)` | [messaging.hpp:88](../wrappers/vst3/source/messaging.hpp) |
| Windows view watcher | `win_view.cpp` | [win_view.cpp:61](../libs/tiny_platform/source/win_view.cpp) |

**Threads the host supplies:** audio (`process`), UI/main (`run_frame`, state
restore, parameter queries), and in several formats a third for state and
`paramsFlush`. Plus AAX's Direct Data wakeup, which the SDK drives at ~30 ms
from a thread we do not own.

**The crossings that matter**, i.e. the ones a race would live in:

- audio → editor: meter queue (`Set_meter`).
- editor → host → audio: `User_action` round-trip.
- state load → audio: the `Lock_free_queue<Set_param>` state queue; CLAP's
  `_from_flush`.
- audio ↔ worker: `From_processor` / `To_processor`.
- editor ↔ worker: `From_editor` / `To_editor`.
- audio → wrapper → host: `_pending_latency` / `_accepted_latency` atomics, the
  hidden VST3 latency parameter, AAX's `latency_seq`.
- audio → shuttle → COM: VST3 `_worker_outbound`.
- audio → Direct Data: AAX `Byte_ring` in private data, drained remotely.
- any → `Task_manager`: `on_main` / `on_background` / `on_serial`.

The good news from the audit: the lock-free code uses real `std::atomic` with
explicit acquire/release, which TSan models exactly. Correct lock-free code
produces **no** TSan reports — so a clean run is meaningful, and a report is
very likely a real finding rather than tool noise. That is the property that
makes this worth building.

---

## 3. Build configuration

### 3.1 Sanitizer scope — configure-wide, not target-wide

[test-suite.md](test-suite.md) §11 proposes `TINY_TESTS_SANITIZE` applying
`-fsanitize=address,undefined` to test targets only, mirroring what
`all_plugins/tests/CMakeLists.txt` already does. **TSan cannot work that way.**
A race between instrumented framework code and an uninstrumented static library
is either missed or misreported; partial instrumentation is the main source of
TSan false positives. The sanitizer must cover every source-built dependency in
the link.

So TSan is a **preset**, not a target property:

```json
{
  "name": "tsan",
  "inherits": "base",
  "generator": "Unix Makefiles",
  "binaryDir": "${sourceDir}/build-tsan",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "RelWithDebInfo",
    "CMAKE_CXX_FLAGS": "-fsanitize=thread -fno-omit-frame-pointer -g -O1",
    "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=thread",
    "CMAKE_SHARED_LINKER_FLAGS": "-fsanitize=thread",
    "CMAKE_OSX_ARCHITECTURES": "arm64",
    "TINY_BUILD_PLUGINS": "OFF",
    "TINY_BUILD_TESTS": "ON",
    "TINY_LOG": "ON"
  }
}
```

Notes on each choice:

- **`RelWithDebInfo` + `-O1`**, not the `-O0` of the `debug` preset. TSan already
  costs 5–15× in time and 5–10× in memory; `-O0` on top makes a chaos run that
  should take 30 s take minutes. `-O1 -g -fno-omit-frame-pointer` keeps stacks
  readable.
- **Single arch.** TSan builds one slice; the demos' `arm64;x86_64` must be
  overridden. (Test targets already opt out of the demos' universal/deployment
  constraints in the `all_plugins` precedent — same rule here.)
- **`TINY_BUILD_PLUGINS=OFF`.** Nothing in the TSan configure should link Skia.
  This is not a limitation to work around, it is the point: see §3.3.
- **`TINY_LOG=ON`.** The log ring is itself a TSan target, and the probe layer is
  how a chaos run explains itself when it does fail. Also means the log drain
  thread participates, which is correct — it is a real thread in shipping Debug
  builds.

A `TINY_SANITIZE=thread|address|undefined|off` cache variable is still worth
having for the *test* targets so ASan/UBSan runs stay per-target the way
`all_plugins` does them, but TSan specifically ignores it and reads the preset.

### 3.2 Instrumentation coverage of the SDKs

Audited from `tiny_deps/cmake/setup_*.cmake`:

| Dependency | How consumed | Instrumented by the tsan preset? |
|---|---|---|
| **CLAP** (`clap`, `clap-helpers`) | header-only INTERFACE targets | ✅ fully |
| **VST3 SDK** | `add_subdirectory` of `third_party/vst3sdk`, built from source | ✅ fully |
| **AAX SDK** | prebuilt `libAAXLibrary.a`; source present at `third_party/aax-sdk` | ❌ — needs a from-source switch |
| **AudioUnitSDK** | prebuilt `libAudioUnitSDK.a`; source present at `third_party/AudioUnitSDK` | ❌ — needs a from-source switch |
| **Skia** | prebuilt `libskia.a`, no source in the tree | ❌ — and no path to fixing it |

This table decides the target order in §5. **CLAP and VST3 are fully
instrumentable today**; AAX and AUv2 need a `TINY_DEPS_BUILD_FROM_SOURCE` option
upstream in `tiny_deps` (both already ship the sources, so it is a CMake change,
not a vendoring change); Skia never will be.

`AUThreadSafeList.h` in the AU SDK is exactly the sort of thing worth
instrumenting, so the AU from-source switch is not academic.

### 3.3 The unit under test is headless

Skia is a prebuilt archive with no source, it spawns its own threads, and the
editor is the only thing that links it. Running TSan across an uninstrumented
graphics stack would produce a noise floor that buries real findings. So:

**The TSan configuration is the headless configuration.** This makes
[headless-plugin.md](headless-plugin.md) a hard prerequisite for the CLAP, AUv2
and AAX-data-model slices — the same dependency [test-suite.md](test-suite.md)
§6 already identified for T2, now with a second reason.

It is **not** a prerequisite for the first two phases, and that is the key
scheduling insight:

- `run_frame` is a template ([tiny_view.hpp:311](../libs/tinyplug/include/tinyplug/tiny_view.hpp))
  and the core only **forward-declares** `SkCanvas`
  ([tiny_view.hpp:19](../libs/tinyplug/include/tinyplug/tiny_view.hpp)). A
  fixture editor whose `on_gui_draw` never dereferences the canvas drives the
  real UI loop with **no Skia link at all**.
- VST3's [audio_effect.hpp](../wrappers/vst3/source/audio_effect.hpp) is
  editor-free; only `controller.hpp` pulls the editor in.
- The AAX algorithm's only window on the world is `Alg_context`
  ([alg_context.hpp](../wrappers/aax/source/alg_context.hpp)) — a struct of
  pointers, no editor, no data model.

So the full three-thread scenario (audio × UI × worker) is reachable **before**
headless lands, at the contract tier, against a fixture. Headless only buys the
wrapper's own cross-thread state.

### 3.4 Runtime configuration

`TSAN_OPTIONS`, set by CTest per test and checked in as
`tests/sanitizers/tsan.options`:

```
halt_on_error=1
history_size=7
second_deadlock_stack=1
suppressions=<abs>/tests/sanitizers/tsan.supp
external_symbolizer_path=<llvm-symbolizer>
```

`history_size=7` (the maximum) is the one that matters — the default keeps too
little history to show the *other* stack in a report, which is the half you need.

---

## 4. The chaos driver

This is the substance. TSan is a **happens-before detector, not a schedule
explorer**: it reports races between accesses that actually executed
concurrently during the run, held in a bounded shadow history. It finds nothing
in code that never ran on two threads at once. The driver's whole job is to make
the interesting crossings actually overlap.

### 4.1 Shape

```
tests/tsan/
  driver.hpp        # Chaos_driver: the thread cast + the schedule
  actions.hpp       # the action vocabulary below
  main.cpp          # --seed --duration --actions --rig
  tsan.supp
  tsan.options
```

A run is: spawn the cast, run a seeded pseudo-random action stream for
`--duration`, tear down while traffic is still in flight, report.

**The cast** — real threads, none of them the main thread except the UI one:

| Thread | Loop |
|---|---|
| `audio` | `process()` at a jittered block size, back to back, no sleeping. Tagged with `TINY_LOG_THREAD(audio)`. |
| `ui` | `run_frame()` at ~60 Hz, plus whatever UI-originated actions the schedule fires. |
| `host_aux` | the format's out-of-band caller: `paramsFlush`, `setState`, parameter queries, `getLatencySamples`. Separate from `ui` deliberately — several hosts do use a third thread, and merging them hides exactly the races that matters. |
| `worker` | the framework's own, not the driver's. |

**The action vocabulary** — each entry is a thing a real host does, with the
thread it must be issued from:

| Action | Thread | Targets |
|---|---|---|
| `automate(addr, lane)` | audio | the delivery path |
| `edit(addr)` (gesture: start/set×n/end) | ui | editor → host → processor round-trip |
| `load_state(chunk)` | host_aux | the state queue, `Undo_history`, `Host_preset_loaded` |
| `flush()` (zero-frame process) | audio or host_aux | CLAP `_from_flush`, VST3 flush shapes |
| `propose_latency(n)` | audio (kernel-driven) | `_pending_latency` / `_accepted_latency` |
| `query_latency()` | host_aux | the acceptance read — the async window |
| `worker_burst(n)` | audio + ui | both worker legs at once |
| `meter_flood()` | audio | the meter queue at overflow |
| `reconfigure(sr, block)` | host_aux, between activations | `configure` superseding a pending proposal |
| `activate/deactivate` | host_aux | shuttle start/stop, worker start/stop |
| `open/close_editor()` | ui | `bind_main`, editor rebind, view lifetime |
| `teardown_dirty()` | driver | destruct with traffic in flight |

**Seeding.** One `std::mt19937` per thread, seeded from `--seed` plus a
per-thread salt, so a failing run reproduces. Print the seed on every run,
failing or not.

**Termination.** The driver deliberately tears down mid-traffic — worker queues
non-empty, an editor open, a latency proposal outstanding. Destructor-order and
use-after-free bugs live there, and it is the one shape validators never test.

### 4.2 Why the existing `Fake_host` is not this

[`all_plugins/tests/harness/fake_host.hpp`](../../all_plugins/tests/harness/fake_host.hpp)
is a good **single-threaded offline** host — it owns a timeline, splits blocks at
event offsets, and reproduces the wrapper process loop. It is the right base for
T1 and it has already been ported to the current `configure`/`Reset` API. But it
runs everything on the calling thread, so under TSan it is inert.

The relationship: `Chaos_driver` **contains** a `Fake_host` and calls it from the
`audio` thread. The timeline, block plan and automation lanes are reused
verbatim; what the driver adds is the other three threads and the action stream.
Do not fork it.

---

## 5. Targets and phasing

Each phase is independently useful and each ends with something runnable.

### Phase 0 — `tests/` exists

`tests/CMakeLists.txt` with `enable_testing()`, `audio_bench` wired in
(`FetchContent` defaulting to `../audio_bench`, mirroring `TINY_DEPS_PATH`), the
`tsan` preset, `tests/sanitizers/`. Nothing TSan-specific yet — this is the
skeleton [test-suite.md](test-suite.md) needs anyway. **No prerequisites.**

### Phase 1 — `tiny_tsan_primitives` (Rig A)

Direct multi-threaded stress of the concurrency primitives, no wrapper, no SDK,
no fixture. Builds in seconds; runs in seconds.

| Under test | Scenario |
|---|---|
| `Lock_free_queue` × all four `Queue_concurrency` modes | N producers × M consumers at capacity, over/underflow, plus **>64 threads** against the `spmc`/`mpsc` thread registry |
| `Notification_queue` / `Serial_queue` | push/pop/`done()` racing with destruction |
| `Task_manager` | `on_main`/`on_background`/`on_serial`/`is_main_thread` from every thread, with `bind_main` racing the first frame |
| `log::Ring` | the Vyukov MPMC bounded ring under many writers + the drain thread, including drop-on-full |
| `Byte_ring` | AAX Direct Data producer at audio rate vs. a consumer driven at wildly irregular intervals with gaps that overflow |

This phase is where the TSan layer earns its keep cheaply, and it needs
**nothing that does not exist today.**

### Phase 2 — `tiny_tsan_<example>` (Rig A, applied to real plug-ins)

The direct answer to "run the examples under TSan." A CMake helper —

```cmake
tiny_add_tsan_target(<name> <plugin_lib_target> <source_dir>)
```

— that takes any `*_lib` example target (or a downstream plug-in), puts its
`source/` on `-I`, and builds a chaos runner around its **processor + models +
worker**, with the framework's real `Worker_runner`, `Task_manager` and meter
queues in the loop, and a stub editor driving the real `run_frame`. One target
per example, for the same reason `sketch_alloc_<product>` is one target per
product: every plug-in's `#include "models/params.hpp"` is unqualified and
resolves against whichever source dir is on `-I`, so two plug-ins can never
share one target's include path.

Applied to what each example actually exercises:

| Target | What it puts under TSan |
|---|---|
| `tiny_tsan_worker_demo` | both worker legs concurrently — the richest case |
| `tiny_tsan_latency_demo` | proposal from the audio thread vs. acceptance from `host_aux` |
| `tiny_tsan_render_mode` | `Render_mode` flips racing `process` |
| `tiny_tsan_automation_tester` | delivery under load; also the probe fixture's ancestor |
| `tiny_tsan_gain_demo` | the minimal baseline — a clean run here is the control |

No wrapper, no Skia, no bundle. **No prerequisites beyond Phase 0.**

### Phase 3 — `tiny_tsan_vst3` (Rig B, processor side)

The first wrapper. `audio_effect.cpp` + `messaging.cpp` compiled against a probe
fixture and driven through `IAudioProcessor` from the chaos driver's threads.
Targets specifically:

- `Outbound_message_shuttle` — the drain thread vs. audio-thread pushes into
  `_worker_outbound`, across `setActive` start/stop cycles.
- `_pending_latency` / `_accepted_latency` vs. `getLatencySamples()` called from
  `host_aux` while `process` runs.
- The `_events` vector and the flush paths.

The `all_plugins` VST3 flush suite moves here in the same change (it is already
marked for the move in its own README), giving the target real assertions
alongside the race check. **No prerequisites beyond Phase 0** — the VST3
processor side is editor-free.

### Phase 4 — `tiny_tsan_aax_alg` (Rig B, algorithm side)

`Alg_context` built by hand, `Alg_state` placed by `alg_init`, coefficient
segments pushed while a consumer thread drains the return `Byte_ring` at
irregular intervals. This is the format whose concurrency model is least like
the others and the one with no validator coverage of its transport.

Blocked only on wanting the AAX SDK instrumented (§3.2) — the algorithm barely
touches the SDK library at runtime, so it can run before that lands, with the
caveat noted in the suppressions file.

### Phase 5 — `tiny_tsan_clap`, `tiny_tsan_auv2`, `tiny_tsan_aax_model`

**After [headless-plugin.md](headless-plugin.md).** CLAP first: header-only SDK,
single component, plain C vtable, fully instrumented — it will be the cleanest
signal in the suite. CLAP's `thread_check` callback is also a free assertion
surface: have the fake host answer *correctly* and assert the wrapper never
violates it, then have it answer *wrongly* as a hostile-host case.

### Phase 6 — Rig C, the bundle-loading host

`tools/tiny_host` — a TSan-built CLI host that loads a TSan-built bundle. CLAP
first (`dlopen` + `clap_entry`), VST3 second (the SDK's hosting module). Same
`Chaos_driver`, different linkage. What this adds over Rig B: entry points,
static initialization order, factory globals, and the real two-component wiring
rather than a hand-assembled one.

Requires the demos to be buildable under the tsan preset without Skia, i.e.
headless bundles — which headless-plugin.md's `TINY_HEADLESS ON` override
already provides.

### Phase 7 — CI

A **scheduled** job, not per-PR: Phase 1 + 2 are fast enough to run per-PR
(seconds), Phases 3–6 are not. Split accordingly:

| Job | Cadence | Runs |
|---|---|---|
| `tsan-fast` | per PR | `tiny_tsan_primitives` + `tiny_tsan_<example>` × 5, 10 s each |
| `tsan-deep` | nightly | every target, 5 min each, fresh seed per night, seed printed |

A nightly with a fresh seed is what turns a bounded-history detector into
coverage over time. Record the seed in the job output so a nightly failure
reproduces exactly.

---

## 6. Predicted first catches

From reading the code rather than from running anything. Listed so the layer can
be judged on whether it finds them.

1. **`Task_manager::_main_id` is a non-atomic `std::optional<std::thread::id>`**
   ([task_manager.hpp:44](../libs/tinyplug/include/tinyplug/task_manager.hpp),
   [task_manager.cpp:7](../libs/tinyplug/source/task_manager.cpp)). Written by
   `bind_main`, which `run_frame` calls **every frame** from the UI thread
   ([tiny_view.hpp:325](../libs/tinyplug/include/tinyplug/tiny_view.hpp)), and
   read by `is_main_thread()`, which `Actor::is_main_thread()` exposes to any
   thread. Today's callers appear to be UI-thread-only, so this is latent rather
   than live — but it is one plug-in author calling `actor().is_main_thread()`
   from a background task away from being a real race, and Phase 1 exercises
   exactly that. Fix is an `std::atomic<std::thread::id>` (already
   `static_assert`-ed lock-free elsewhere in the tree).

2. **`queue_impl::Thread_registry` bounds guard is dead.**
   [lock_free_queue.hpp:116-121](../libs/tinyplug/include/tinyplug/lock_free_queue.hpp):

   ```cpp
   auto own_slot = num_threads.fetch_add(1, std::memory_order_relaxed);
   if (own_slot >= num_threads) { assert(false); own_slot = 0; }
   ```

   `own_slot` is the pre-increment value and `num_threads` is the post-increment
   value, so the condition is `n >= n+1` — always false. The guard can never
   fire, and the 65th thread to touch an `spmc`/`mpsc` queue writes past
   `infos[64]`. Should compare against `max_threads`. Not a race, so TSan won't
   report it directly, but the ">64 threads" scenario in Phase 1 is what walks
   into it, and ASan on the same test is what names it.

3. **`log::Probe` construction on the audio thread.** [CLAUDE.md](../CLAUDE.md)
   documents that probes must be constructed off the audio thread because the
   constructor performs the one-time layer init. That is a documented convention
   with no enforcement; a chaos run that constructs a probe from the audio
   thread will show the init race, which is the cheap way to turn the convention
   into a test.

4. **Shuttle / worker start-stop cycles.** `Outbound_message_shuttle::start` is
   called from `setActive(true)` and `stop` from `setActive(false)`
   ([audio_effect.cpp:182-187](../wrappers/vst3/source/audio_effect.cpp)); the
   `_drains` vector is populated once at construction, which is safe as written
   — but the driver's `activate/deactivate` action is what proves it stays safe.
   Same shape for `Worker_runner::start`/`stop`.

A clean run against the rest would itself be a useful result: it would mean the
farbot-derived queues and the latency handshake atomics are correct as written,
which is currently believed rather than known.

---

## 7. What TSan does not cover

Stated so the layer isn't oversold, and so the sibling work is scheduled rather
than assumed.

- **Realtime safety.** TSan says nothing about allocation, locks or syscalls on
  the audio thread. That is a separate trap, and the prior art already exists:
  [`all_plugins/tests/allocation/probe.hpp`](../../all_plugins/tests/allocation/probe.hpp)
  is a counting global `operator new` armed around the code under test, with one
  target per plug-in. **Port it alongside Phase 2** — same `tiny_add_*_target`
  shape, same per-example target list, and the two together are the pair that
  matters: TSan says "no races", the RT trap says "no allocations."
- **Missed-sync bugs on paths the driver never overlaps.** Coverage is a function
  of the action vocabulary. Adding an action is how coverage grows; a nightly
  fresh seed is how the schedule space gets sampled.
- **Uninstrumented code.** Skia always, AAX/AU SDKs until the from-source switch.
  Each is a named entry in the suppressions file with a reason, not a silent gap.
- **Real host behaviour** — priority inversion, RT deadline misses, a host
  calling in an order no simulated host thought to try. That stays Tier 4 in
  [test-suite.md](test-suite.md), and the validators stay Tier 3.
- **Lock-order inversions** are reported where the platform's TSan supports it,
  but this codebase is mostly lock-free, so it is a bonus rather than a goal.

---

## 8. Suppression discipline

`tests/sanitizers/tsan.supp`, with the same rule the validator manifest in
[test-suite.md](test-suite.md) §9 uses: **every entry names a reason and stays
reviewable.**

```
# Skia is a prebuilt archive with no source in tiny_deps; it cannot be
# instrumented, and its internal atomics are invisible to TSan.
called_from_lib:libskia.a

# AAX SDK is consumed as a prebuilt archive (tiny_deps/prebuilt/aax-sdk).
# Remove once TINY_DEPS_BUILD_FROM_SOURCE lands.
called_from_lib:libAAXLibrary.a
```

Two rules:

1. A suppression for **our own code** requires either a `__tsan_acquire` /
   `__tsan_release` annotation explaining the custom synchronization, or a
   fix. "Benign race" is not an accepted reason — the codebase currently has no
   hand-rolled synchronization that would need one, and it should stay that way.
2. Suppressions for third-party archives are temporary by construction and each
   carries the condition under which it is deleted.

---

## 9. Relationship to test-suite.md

[test-suite.md](test-suite.md) stays the master plan and the invariant
catalogue. This document replaces its two TSan lines (§5.13 and the Phase 9 row)
and reorders the front of its phasing:

| test-suite.md phase | Change |
|---|---|
| Phase 1 (`tests/` skeleton + T0) | Unchanged — it is Phase 0 here, shared |
| Phase 9 (RT trap + TSan, last) | **Moves to Phases 1–2 here**, before T1/T2 |
| Phases 2–8 | Unchanged in content; TSan targets slot in beside each tier's targets rather than after all of them |

The argument for the reorder: T1 and T2 are large (fixtures, `Reference_host`,
five per-format fake hosts, golden traces) and they pin *behaviour*. The TSan
layer is small, pins *safety*, and the safety bugs are the ones that reach a
customer as an intermittent crash nobody can reproduce. Building it first also
forces the `tests/` skeleton, so it costs the rest of the suite nothing.

---

## 10. Open decisions

1. **Instrument the AAX and AU SDKs?** Both ship sources in `tiny_deps/third_party`
   but are consumed as prebuilt archives. Recommendation: add
   `TINY_DEPS_BUILD_FROM_SOURCE` to `tiny_deps` (default OFF, ON in the tsan
   preset). It is a CMake change, not a vendoring change, and it also serves a
   future ASan/UBSan run.
2. **Chaos duration and CI budget.** Recommendation: 10 s per target per PR,
   5 min per target nightly, fresh seed nightly. Long enough to overlap
   everything in the action vocabulary; short enough that `tsan-fast` stays under
   two minutes.
3. **Does the driver live in `tests/tsan/` or in `tests/harness/`?**
   Recommendation: `tests/harness/chaos_driver.hpp` — it is the multi-threaded
   sibling of `reference_host.hpp` and the RT trap will want it too. `tests/tsan/`
   holds only the sanitizer config and the target list.
4. **Windows.** MSVC has no TSan; clang-cl's is not usable in this configuration.
   Recommendation: state TSan as macOS-only (and Linux-only when that lands),
   and cover Windows with the RT trap plus ASan, which MSVC does support.
5. **Fixture or example?** Phase 2 drives real examples; Phase 3+ needs the
   `probe` fixtures from [test-suite.md](test-suite.md) §4. Recommendation: use
   examples for Phase 2 (they exist, and "run the examples under TSan" is the
   literal ask), and let the probe fixtures arrive with T1 as already planned —
   the chaos driver takes either.

---

## 11. Key files

| File | Role |
|---|---|
| [libs/tinyplug/include/tinyplug/lock_free_queue.hpp](../libs/tinyplug/include/tinyplug/lock_free_queue.hpp) | Every queue mode; the thread registry bug in §6.2 |
| [libs/tinyplug/include/tinyplug/task_manager.hpp](../libs/tinyplug/include/tinyplug/task_manager.hpp) | The `_main_id` candidate in §6.1 |
| [libs/tinyplug/include/tinyplug/tiny_worker.hpp](../libs/tinyplug/include/tinyplug/tiny_worker.hpp) | `Worker_runner` — the worker thread |
| [libs/tinyplug/source/tiny_log.cpp](../libs/tinyplug/source/tiny_log.cpp) | The MPMC ring and the drain thread |
| [libs/tinyplug/include/tinyplug/tiny_view.hpp](../libs/tinyplug/include/tinyplug/tiny_view.hpp) | `run_frame` — the UI thread's loop; `SkCanvas` is only forward-declared here |
| [wrappers/vst3/source/messaging.hpp](../wrappers/vst3/source/messaging.hpp) | `Outbound_message_shuttle` |
| [wrappers/aax/source/byte_ring.hpp](../wrappers/aax/source/byte_ring.hpp) | Direct Data transport |
| `tiny_deps/cmake/setup_*.cmake` | The instrumentation-coverage table in §3.2 |
| [`all_plugins/tests/harness/fake_host.hpp`](../../all_plugins/tests/harness/fake_host.hpp) | The single-threaded host the driver wraps |
| [`all_plugins/tests/allocation/probe.hpp`](../../all_plugins/tests/allocation/probe.hpp) | The RT trap to port alongside Phase 2 |
| [plans/headless-plugin.md](headless-plugin.md) | Prerequisite for Phases 5–6 |
| [plans/test-suite.md](test-suite.md) | The master plan this is the first slice of |
