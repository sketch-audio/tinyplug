# Plan: the tinyplug test suite

> Status: **design.** tinyplug currently ships zero automated tests. This plan
> defines a five-tier suite whose centre of gravity is **wrapper conformance** —
> driving each format wrapper through its real host-facing API with a fake host,
> so the guarantees the framework makes to a client plug-in are pinned by
> executable assertions rather than by having shipped without complaints.
>
> Prior art: [`all_plugins/tests/`](../../all_plugins/tests/) already runs a
> probe-fixture harness against the *adapter* layer, plus one wrapper-level
> suite (`sketch_tests_vst3`, the VST3 parameter-flush test) whose own README
> says *"This belongs in tinyplug, beside the wrapper it tests."* That suite is
> the seed of Tier 2 here.

---

## 1. Goal and non-goals

**Goal.** A client plug-in author writes one `plugin::Processor` and one
`plugin::Editor` and gets five binaries. Everything between the host's API and
`handle`/`process` is framework code the author cannot see, cannot debug,
and has to trust. This suite is the statement of what that code guarantees:

- **Every format delivers the same events, in the same order, at the same sample
  offsets, for the same host behaviour.** A kernel that is correct under one
  wrapper is correct under all five (modulo documented per-format limits — AAX
  block-start delivery, AUv2 coalescing).
- **Every format survives the call sequences real hosts actually make**, including
  the malformed, out-of-order and degenerate ones the SDK validators don't cover.
- **State, latency, meters, worker traffic and undo round-trip identically** across
  formats.
- **Format validators pass**, reproducibly, from one command, in CI.

**Non-goals.**

- Testing user DSP. That belongs downstream (`all_plugins/tests/golden_audio/`).
  tinyplug's fixtures exist to *observe the wrapper*, not to make sound.
- Testing Skia rendering or pixel output. The editor is exercised through
  `run_frame`'s event/action semantics with a null canvas, not by drawing.
- Replacing the manual DAW pass. Tier 4 stays a checklist; the point of Tiers 0–3
  is to make that checklist short and rare.

---

## 2. What exists today

| Asset | Where | Reusable as |
|---|---|---|
| `automation_tester` demo — one linear gain param, **outputs its param value as DC** | [examples/automation_tester/](../examples/automation_tester/) | The probe fixture, near-verbatim (§4) |
| Probe harness: `Fake_host`, `Automation_lane`, `Trace`, `Delivery::{Sample_accurate,Block_start,Immediate_only}`, `Block_plan::jitter` | `all_plugins/tests/harness/` | Tier 1 harness, ported and de-`sketch`-ed |
| VST3 flush suite — drives real `tiny::vst3::Audio_effect` through `IAudioProcessor` with hand-built `ProcessData`, 5 flush block shapes × 2 assertions | `all_plugins/tests/source/test_vst3_flush.cpp` + `fixtures/vst3/` | Tier 2 VST3 seed — **move it here** |
| `audio_bench` — single-header registry (`Tests::add`), `Error::*` tolerances, signal generators, `add_expected_failure` (XFAIL/XPASS) | [../audio_bench](../../audio_bench) | The test framework for all tiers |
| AAX validator run procedure + known-good baseline (all six demos, 2026-07-23) | [aax-validation-checklist.md](aax-validation-checklist.md) §1 | Tier 3 AAX |
| `clap-validator` binary, `auval`, `pluginval.app` | `../clap-validator/binaries`, `/usr/bin`, `/Applications` | Tier 3 |
| Manual DAW checklist (18 sections) | `all_plugins/plans/test-plan.md` | Tier 4 — generalize the framework-level half back into tinyplug |

Nothing in tinyplug's own CMake calls `enable_testing()` yet.

---

## 3. The five tiers

```
T4  manual DAW matrix          — hours, human, per release
T3  bundle validators          — minutes, real bundles, per PR (macOS runner)
T2  wrapper conformance        — seconds, real wrapper class + fake host   ← the payoff
T1  reference-host contract    — seconds, processor only, no wrapper
T0  core unit tests            — <1s, pure functions
```

The rule that keeps the pyramid honest: **an invariant is asserted at the lowest
tier that can actually observe it.** Value conversions are T0. "A ramp lands on
target regardless of block size" is T1 — no wrapper can affect it. "A flush with
zero frames still manifests its parameter" is *only* observable at T2, because
the bug lives in the wrapper's block loop. Anything needing a real bundle on disk
(Describe tables, plist, entry point, code signing) is T3.

### T0 — core unit tests

Target `tinyplug_tests_core`. Links `tiny_shared_lib` + `audio_bench` only;
builds in a couple of seconds; no SDKs, no platform lib.

| Unit | File | What to pin |
|---|---|---|
| `Value_helper` | [value_helper.cpp](../libs/tinyplug/source/value_helper.cpp) | All five semantics × three spaces, round-trip identity, every `Knob_adapter` (`Adapt_lin/log/pow/taper/piece`) forward+inverse, endpoint exactness (knob 0 → `min_val`, knob 1 → `max_val` **exactly**), `default_value(Fixed, Host)` quantization, domain asserts fire where documented. Called out as the one logic-dense untested piece in [refactor-ideas.md](refactor-ideas.md). |
| `params::Model` tree | [tiny_params.hpp](../libs/tinyplug/include/tinyplug/tiny_params.hpp) | `validate_tree` accepts a good tree; rejects empty identifier, duplicate sibling identifier, colliding global keypath. Flatten order == address order. Keypath construction skips the root group. |
| `Param_order` | ditto | `Au_ordinal` from an `Au_ordered` model; `Indexable` fallback when `au_order()` absent; **never** tree order (§8). |
| `State_adapter` | [state_adapter.cpp](../libs/tinyplug/source/state_adapter.cpp) | JSON round-trip for all four `State_map` variant types; missing `version`; unknown keys ignored; unknown param address ignored; NaN/inf rejection; nested group nesting matches keypath chain. |
| `Undo_history` | [undo_history.cpp](../libs/tinyplug/source/undo_history.cpp) | Gesture pairing (`Action_start`/`Action_end` → one step); coalescing within a gesture; `push_host_load` one coalesced step; `amend_host_load` folds a marker into the open step; depth limit; redo invalidation on new edit; empty-stack queries. |
| `Lock_free_queue` | [lock_free_queue.hpp](../libs/tinyplug/include/tinyplug/lock_free_queue.hpp) | Each `Queue_concurrency` mode: FIFO order, full-queue behaviour, no loss under a threaded producer/consumer stress (also the TSan target). |
| `Action_queue`, `Change_list`, `Notification_queue`, `Serial_queue` | `libs/tinyplug/source/` | Ordering, capacity, drain semantics. |
| `Host_formatter` | [host_formatter.cpp](../libs/tinyplug/source/host_formatter.cpp) | value→text→value round-trip per semantic and unit; locale independence; truncation at the SDK's buffer sizes (VST3 128 `TChar`, CLAP `size`, AU `CFString`). |
| `Gesture_recognizers` | [gesture_recognizers.cpp](../libs/tinyplug/source/gesture_recognizers.cpp) | Drag/double-click/shift-fine state machines, synthetic pointer streams. |
| `tiny_dsp` | [libs/tiny_dsp/](../libs/tiny_dsp/) | `Linear_ramper` (lands exactly, block-size independent), `Delay_line` (readback identity, wrap), `Host_bypass` (crossfade shape, `set_latency` alignment, PDC delay = reported latency). |
| `Byte_ring` | [byte_ring.hpp](../wrappers/aax/source/byte_ring.hpp) | Push/read byte ranges, wrap, **full push drops rather than overwrites** (the property Direct Data's lock-free-ness rests on), reader sees only whole records. Header-only and AAX-SDK-free → belongs in T0 despite living under `wrappers/aax/`. |

### T1 — reference-host contract

Target `tinyplug_tests_contract`. Drives a fixture `Some_plug_processor`
**directly**, through a `Reference_host` that reproduces the wrapper process
loop: split the buffer at each event offset, `handle()` between sub-calls,
correct `Musical_context::sample_pos` per sub-block, echo `propose_latency` back
as `Accepted_latency`.

This tier's job is to **define** the contract that T2 then checks each wrapper
against. The VST3 wrapper is the reference implementation
([audio_effect.cpp](../wrappers/vst3/source/audio_effect.cpp) `process`), so
`Reference_host` is modelled on it with the host quirks removed and re-introduced
deliberately:

- `Delivery::Sample_accurate` — VST3 / CLAP / AUv3.
- `Delivery::Block_start` — AAX (no sub-block splitting; every point applied at
  offset 0).
- `Delivery::Immediate_only` — AUv2 as Logic actually drives it (one coalesced
  value per param per block).

Automation is authored in **absolute timeline samples**, not buffer offsets, so
"same lane, eight block sizes, diff the traces" is a controlled experiment.
`Block_plan::jitter` catches anything keyed on `num_frames`.

### T2 — wrapper conformance (the payoff)

One target per format, each compiling the **real wrapper sources** against a
fixture plug-in and driving them through the format's host-facing API with a fake
host. Detailed per-format design in §6; the invariant catalogue they execute is
§5.

### T3 — bundle validators

A `tools/validate.py` driver that builds the demo bundles and runs
`auval` / `pluginval` / `clap-validator` / `aax-validator` over each, normalizes
their output, diffs against a checked-in expected-result manifest (so a *known*
failure like "no page tables registered" doesn't have to be re-triaged every run),
and exits non-zero on anything new. §9.

### T4 — manual DAW matrix

A framework-level checklist (install, load, automate, save/recall, undo, preset,
latency, bypass, resize, two instances, sample-rate/buffer-size sweep, close/reopen
×10) per format per DAW. Generalized from `all_plugins/plans/test-plan.md`
§§1–15 with the product-specific half dropped. Lives at
`plans/manual-daw-checklist.md`, not in this suite's CMake.

---

## 4. Fixtures

Fixtures are plug-ins written to be *observed*, not to sound good. Four of them,
under `tests/fixtures/`:

### `probe` — the workhorse

Lifted from `automation_tester`: **the output is the parameter state**. Left
channel carries the observed param's manifested value as DC, so the output buffer
is a complete per-sample record of the param trajectory. Right channel carries a
one-sample pulse on each `process` entry — the update grid, directly observable
with no instrumentation inside the wrapper.

Its param model deliberately covers one of everything:

| Param | Purpose |
|---|---|
| `lin_real` | `Real` + `Adapt_lin` — the plain baseline, host space ≠ plain space |
| `log_real` | `Real` + `Adapt_log` — non-linear knob mapping across the wrapper boundary |
| `int_p`, `fixed_p` | integer / fixed semantics (host space == plain space) |
| `list_p` | 5-item list — step counts, `GetParameterValueStrings`, CLAP enum flags |
| `bool_p` | 2-step, the AAX pseudo-bypass's neighbour |
| `hidden_p` | `Policy::Hidden` — persists, no host lane |
| `interface_p` | `Policy::Interface` — no persist, no lane |
| `control_p` | `Policy::Control` — host UI, no automation lane |
| `wide_p` | `Real` with >2048 steps — the AAX validator clamp |
| grouped params | two nested group levels, to exercise clumps / units / modules / keypaths |

Meters: one per `Meter_policy` (`peak`, `stream`, `trig`).

### `settling_probe`

Same, but the observed param runs through a 50 ms one-pole. This is what
separates *delivered* from *manifested*: dropped stays at the old value forever,
delivered-but-not-resynced glides up from it, manifested is on target at the
first sample. Required by the flush tests (§5.3) and by every "zero-frame call"
case, where there is no interval over which a value could legitimately glide.

### `full_probe`

`probe` plus a worker (`worker.hpp` present), a latency-proposing path, a tail,
sidechain enabled, and an editor. The fixture for the worker channel (§5.8),
latency protocol (§5.6) and editor path (§5.9).

### `bare_probe`

Minimum legal plug-in: one param, no worker, no editor, no meters, no sidechain,
zero latency. Pins that every `#if TINY_HAS_WORKER` / `TINY_HAS_EDITOR` /
optional-model path compiles and runs with everything absent — the configuration
most likely to rot, since no demo uses it.

**Fixtures need `plug_info.hpp`.** Generate it through the real
`configure_plug_info` so the test path exercises the same CMake the demos do,
rather than hand-writing it as the `all_plugins` VST3 fixture does today.

---

## 5. The invariant catalogue

This is the substance of the plan: the list of things a client plug-in is entitled
to assume. Each row names the tier that can observe it and the formats it applies
to. `†` marks an invariant that a format validator does **not** cover, which is
the main reason this suite has to exist.

### 5.1 Lifecycle and call sequence

| Invariant | Tier | Formats |
|---|---|---|
| `reset(sr)` is called before the first `process`, and again after any sample-rate change — never *during* processing | T2 | all |
| Construction → `initialize`/`init` → bus/port config → `setupProcessing`/`activate` → `setActive(true)`/`startProcessing` → `process`… → stop → deactivate → `terminate` → destruct. Each wrapper survives the full sequence and every **legal early exit** from it (terminate without activate; activate/deactivate with no process; double `setActive(true)`) | T2 | all |
| Re-activation after deactivate resets DSP state but **preserves parameter values** | T2 | all |
| `setupProcessing` with a changed sample rate between two activations propagates a new `reset(sr)` † | T2 | all |
| `setupProcessing` with a changed `maxSamplesPerBlock` does not reallocate on the audio thread and does not lose params † | T2 | VST3, CLAP, AUv2, AAX |
| A `process` call is never made without a preceding activate; a wrapper asked to process while inactive returns cleanly rather than UB † | T2 | all |
| Destruction while the editor is open, and while worker traffic is in flight, is clean (the reentrancy-guard/destructor-order work in `e322d78`) † | T2 + ASan | all |
| Two instances in one process share no state (statics, ObjC class names, AAX `Alg_state`) † | T2 | all |
| `setState` before `setActive`, and `setState` **while active** (a host loading a preset during playback) both work | T2 | all |

### 5.2 Parameter delivery and ordering

| Invariant | Tier | Formats |
|---|---|---|
| An automation point at timeline sample *N* is delivered to `handle` such that the *N*th sample of `process` sees the new value — for every block size, and for a block boundary landing exactly on *N* | T1+T2 | VST3, CLAP, AUv3 |
| Multiple points in one block are delivered in ascending offset order, with `process` split between them | T1+T2 | VST3, CLAP, AUv3 |
| Two points at the **same** offset preserve host order | T2 | VST3, CLAP |
| A point at offset 0 is applied *before* the block's first sample, not after | T1+T2 | all |
| A point at offset == `numSamples` (one past the end — hosts do emit these) is not dropped and not applied to this block's samples † | T2 | VST3, CLAP |
| `Ramp_param` reaches its target at exactly `dur_samples`, and the trajectory is block-size independent | T1 | all |
| A ramp interrupted by a new `Set_param` retargets from its current value, not its start value | T1 | all |
| Under `Delivery::Block_start` (AAX) and `Immediate_only` (AUv2), values still *converge* — the endpoint after the lane is identical, only the path differs | T1 | AAX, AUv2 |
| The `_events` vector never exceeds its reserved capacity for the worst legal host input (one point per param per block, plus wrapper-authored events) — no audio-thread allocation | T2 + RT check | all |
| Host param values arrive in **host space** and reach the kernel in **plain space**, through the correct `Value_helper` call for the semantic (the documented common bug shape) | T2 | all |
| A param edit from the editor reaches the kernel with the same value it left the editor with, after the host round-trip | T2 | all |

### 5.3 Parameter flush / zero-frame calls †

The suite `all_plugins` already has, generalized. A host may call the process
entry point with **no audio** purely to deliver parameter changes while idle.

| Invariant | Tier | Formats |
|---|---|---|
| Events on a zero-frame call are **delivered** to the kernel (not parsed and dropped) | T2 | VST3, CLAP, AUv2, AAX |
| Events on a zero-frame call are **manifested** — a `snap()` follows, so the idle stretch's edits don't arrive as a ramp across the head of the next real render | T2 | VST3, CLAP, AUv2, AAX |
| All five VST3 flush block shapes: null bus arrays; zero-channel bus arrays; `numSamples == 0` with complete buses; `numChannels == 0`; null channel pointers on a full-length block | T2 | VST3 |
| CLAP `paramsFlush` (called outside `process`, on the main *or* audio thread) delivers and manifests, and interleaves correctly with the `_from_flush` queue on the next `process` | T2 | CLAP |
| AUv2 `SetParameter`/`ScheduleParameter` with no intervening `Render` | T2 | AUv2 |
| AAX `UpdateParameterNormalizedValue` with no `GenerateCoefficients` in between, and the `TimerWakeup` runtime packet path | T2 | AAX |

Why this is its own section: the VST3 SDK validator's three `FlushParamTest`s build
their points with `for (pos = 0; pos < numSamples; pos++)`, so at `numSamples == 0`
they deliver **none**, and only assert `process` returns `kResultOk`. A green
validator says nothing here.

### 5.4 Buses, channels and buffers

| Invariant | Tier | Formats |
|---|---|---|
| Mono, stereo, and mono-in/stereo-out configurations each render correctly and report the right bus arrangement | T2 | all |
| Sidechain: present-and-connected, present-but-disconnected (null pointers), and absent-at-compile-time (`wants_sidechain == false` leaves the field out of the context struct — `f81158f`) † | T2 | VST3, CLAP, AUv2, AAX |
| In-place processing (host passes the same pointer for input and output) is either handled or explicitly refused † | T2 | VST3, AUv2 |
| Silent-input flags (`AudioBusBuffers::silenceFlags`, CLAP `constant_mask`) are not trusted as "skip the block" without also clearing outputs † | T2 | VST3, CLAP |
| Block size varies every call (jitter 1…`maxSamplesPerBlock`) with no state corruption; **`numSamples == 1`** works | T1+T2 | all |
| A block larger than the declared `maxSamplesPerBlock` (hosts do this) does not overrun † | T2 | all |
| Output buffers are fully written or explicitly zeroed — no uninitialized reads (MSan/valgrind-detectable) | T2 + sanitizer | all |
| Denormal guard is installed for the duration of `process` and restored after | T2 | all |

### 5.5 State and presets

| Invariant | Tier | Formats |
|---|---|---|
| `getState` → `setState` round-trips every persistent param bit-exactly, and the editor `State_map` for all four variant types | T2 | all |
| `Policy::Interface` params are **not** persisted; `Policy::Hidden` params **are** | T2 | all |
| The framework/manufacturer/plugin sentinel words are written and checked; a chunk from a different plug-in is rejected rather than misapplied † | T2 | all |
| A **truncated**, **empty**, **oversized**, or **garbage** chunk is rejected without crash or partial application † | T2 | all |
| Forward compat: a chunk with *more* params than the build knows ignores the extra; **backward** compat: fewer params leaves the rest at default † | T2 | all |
| VST3's split state: `getState`(processor) ↔ `setState`(processor), `getState`(controller) ↔ `setState`(controller), and `setComponentState` establishes the controller's param mirror. Loading a controller chunk into the processor is rejected † | T2 | VST3 |
| A host load pushes **exactly one** coalesced undo step, whether or not the editor is open | T2 | all |
| `Host_preset_loaded` fires **once per load**, synchronously from the restore path, with correct `changes` and `params` spans; two closed-editor loads produce two notifications | T2 | all |
| `add_param` inside `notify` applies through the normal path *and* folds into the same undo step | T2 | all |
| VST3 only: the step opens in `setComponentState` and the notify dispatches at the end of `setState`, once, guarded by the pending flag † | T2 | VST3 |
| Bundled JSON presets load through `State_adapter` identically across formats — same file, five wrappers, same resulting param vector † | T2 | all |
| AAX `CompareActiveChunk` reports equal for an unmodified chunk and unequal after any persistent param moves | T2 | AAX |

### 5.6 Latency protocol

The five-step protocol is identical across formats by design, which makes it a
natural conformance test: one script, five wrappers.

| Invariant | Tier | Formats |
|---|---|---|
| `propose_latency` → format-native signal → host queries → `Accepted_latency{N}` reaches the kernel on the next `process`, with `N` == what the host was told | T2 | all |
| The **sequence, not the value**, gates application — a zero-initialized AAX runtime packet must not read as "the host accepted zero" | T2 | AAX |
| A host that **never acks** (several real ones don't until transport stops) leaves the plug-in in a defined state and does not re-propose in a loop | T1+T2 | all |
| A proposal equal to the currently reported latency is dropped (all wrappers do this) — and the kernel is not left waiting for an ack that will never come | T1+T2 | all |
| `Host_bypass::set_latency` tracks the accepted latency, so soft-bypass PDC stays aligned | T1 | all |
| VST3's hidden latency output-param → `restartComponent(kLatencyChanged)` → `getLatencySamples` → `setActive` toggle round-trip completes across the `kDistributable` boundary † | T2 | VST3 |
| Latency proposed on the *very first* `process` (before the host has ever asked) is not lost | T2 | all |
| `tail_samps()` is reported to the host and is realtime-safe (no allocation, no lock) | T2 + RT check | all |

### 5.7 Meters

| Invariant | Tier | Formats |
|---|---|---|
| A meter written during `process` is observable by the editor within one frame, per its `Meter_policy` — `peak` holds max until read, `stream` delivers every update in order, `trig` latches | T1+T2 | all |
| Meter queue overflow (editor not draining) drops rather than blocks or grows | T1 | all |
| VST3 meters travel as output parameter changes in the `export_param_offset` range, and `setParamNormalized` denormalizes them back correctly on the controller side † | T2 | VST3 |
| A meter value is never mistaken for a param value at the ID boundary (`export_param_offset - 1` / `+ 0`) † | T2 | VST3 |

### 5.8 Worker channel

| Invariant | Tier | Formats |
|---|---|---|
| `From_processor` → worker → `To_processor` completes and the reply drains on the processor's next `process`; `From_editor` → worker → `To_editor` drains in `run_frame` | T1+T2 | all |
| Reply handlers are concept-detected: a processor without `handle_worker_reply` compiles and the drain is a no-op (`bare_probe`) | T0 (compile) | all |
| No allocation on the audio thread for any worker push | T2 + RT check | all |
| Queue-full on the audio thread drops rather than blocks | T1 | all |
| VST3 marshalling: `send_variant`/`reconstruct_variant` round-trips every alternative through a real `IMessage`, including the `Outbound_message_shuttle` thread and the controller's `set_post_cycle` return leg † | T2 | VST3 |
| `Message_router` dispatches by string ID and ignores unknown IDs (host traffic) † | T2 | VST3 |
| Worker shutdown during in-flight traffic is clean (no use-after-free) | T2 + ASan | all |

### 5.9 Editor, actions and undo

| Invariant | Tier | Formats |
|---|---|---|
| `run_frame` order is: drain meters → `on_gui_draw` → observe actions for undo → dispatch actions → reset meter state. Changing it changes it for everyone | T1 | all |
| A drag gesture emits `Action_start` / *n* × `Set_param` / `Action_end` and produces **one** undo step | T1+T2 | all |
| The wrapper translates that into the host's native begin/edit/end (`beginEdit`/`performEdit`/`endEdit`, CLAP `GESTURE_BEGIN`/`VALUE`/`GESTURE_END`, `AUParameterSet` with the right change type, AAX `SetParameterNormalizedValue` inside `TouchParameter`/`ReleaseParameter`) † | T2 | all |
| `Request_resize` reaches the host's resize API and a host that refuses is handled | T2 | all |
| Undo/redo replay pushes `Action_start`/`Set_param`/`Action_end` back through the host, so the host records the change too | T2 | all |
| An editor opened, closed and reopened 10× leaks nothing and rebinds cleanly | T2 + ASan | all |
| `notify(Dark_mode_changed)` is delivered and does not touch view resources when the window is closed | T2 | all |

### 5.10 Transport and musical context

| Invariant | Tier | Formats |
|---|---|---|
| `sample_pos` advances by exactly `num_frames` across a block and is correct for each **sub-block** after an event split | T1+T2 | all |
| `beat_pos` matches `frames_to_beats(sample_pos, tempo, sr)` within tolerance; a tempo change mid-timeline is reflected | T1+T2 | all |
| Loop/cycle: a backwards jump in `sample_pos` is passed through, not smoothed | T2 | all |
| Transport stopped / moving / recording flags map correctly from each host API | T2 | all |
| A host supplying **no** musical context (VST3 `processContext == nullptr`, CLAP `transport == nullptr`) yields defined defaults, not garbage † | T2 | VST3, CLAP |
| `Render_mode::Offline` is set for bounce/freeze where the format signals it, `Realtime` otherwise | T2 | VST3, CLAP, AUv2 |

### 5.11 Bypass

| Invariant | Tier | Formats |
|---|---|---|
| Host bypass produces bit-exact input at the output, delayed by exactly the reported latency | T1+T2 | all |
| Engage/disengage crossfades without a click, and the crossfade is block-size independent | T1 | all |
| Bypass state persists (`tinyplug-host-bypass` in AAX; the equivalent in each format) | T2 | all |
| AAX: the bypass pseudo-parameter at `bypass_address == num_params` packs into the coefficient segments like any other value, and the data model tracks no bypass state | T2 | AAX |

### 5.12 Parameter permanence

Cheap, high-value, and it delivers most of what [param-lockfile.md](param-lockfile.md)
defers. See §8.

### 5.13 Realtime safety

| Invariant | Tier |
|---|---|
| No allocation, free, lock, syscall, or file I/O on the audio thread — under automation load, state load, worker traffic, meter push, and latency proposal | T2, via an allocation-trapping hook armed for the duration of `process` |
| No data race between audio thread, UI thread and worker thread | T2 under TSan |
| No UB in the conversion paths (`-fsanitize=undefined`), no read past an unset optional model | T0/T1/T2 under UBSan |

---

## 6. Per-format T2 harness design

The obstacle is uniform: four of the five wrappers' processor-side headers
transitively include the editor, and therefore Skia and `tiny_platform`.

| Wrapper | Processor-side header | Pulls in editor/Skia? |
|---|---|---|
| VST3 | [audio_effect.hpp](../wrappers/vst3/source/audio_effect.hpp) | **No** — `controller.hpp` does ([controller.hpp:7,10](../wrappers/vst3/source/controller.hpp)) |
| AAX (algorithm) | [alg_proc.hpp](../wrappers/aax/source/alg_proc.hpp) → [alg_context.hpp](../wrappers/aax/source/alg_context.hpp) | **No** |
| AAX (data model) | [parameters.hpp](../wrappers/aax/source/parameters.hpp) | Yes — `editor.hpp` |
| CLAP | [plugin.hpp](../wrappers/clap/source/plugin.hpp) | Yes — `view.hpp`, and the editor is constructed in the ctor |
| AUv2 | [effect.hpp](../wrappers/auv2/source/effect.hpp) | Yes — `editor.hpp` + `view.hpp` |
| AUv3 | ObjC++ extension | Yes |

**[headless-plugin.md](headless-plugin.md) is the enabler and should land first.**
It already delivers exactly what's needed: a CMake-computed `TINY_HAS_EDITOR=0`
that excludes `view.*`, skips `configure_mac_view`, and drops the
`tiny_platform`/Skia link, with the `#if TINY_HAS_EDITOR` gates on each wrapper's
`_editor` member already enumerated per format. With it, **every** wrapper
compiles into a plain test executable.

Ordering consequence: build T0, T1, and the VST3 and AAX-algorithm slices of T2
now (they need nothing new); land headless; then CLAP, AUv2 and the AAX data
model follow immediately. Do **not** invent a second, test-only editor-stubbing
mechanism — that's a parallel configuration nobody ships.

Editor-path tests (§5.9) need the opposite: `TINY_HAS_EDITOR=1` with a **null
graphics backend**. Add a `TINY_PLATFORM_NULL` variant of `tiny_platform`
exposing the same `Platform_view`/`Platform_dialogs` surface with no-op
implementations and a headless `SkSurface` (or a forward-declared `SkCanvas*` that
is never dereferenced — the core already only forward-declares it). This is a
smaller lift than it sounds and it is also what a future Linux/CI-render path
wants.

### VST3 — `tinyplug_tests_vst3`

The seed already exists. Compile `audio_effect.cpp` + `messaging.cpp` (+ with
headless, `controller.cpp` and `entry.cpp`) against the probe fixtures and link
`tiny::vst3sdk` + `sdk_hosting`. `Fake_vst3_host` grows from the `all_plugins`
version into a general host: transport, `IParameterChanges` in and out,
`IEventList`, bus arrangements, `IComponentHandler` capturing
`beginEdit`/`performEdit`/`endEdit`/`restartComponent`, `IMessage` plumbing
between the two components, and `IBStream` state via `MemoryIBStream`.

The two-component split is the headline: a `Fake_vst3_host` that *deliberately*
keeps processor and controller in separate objects with no shared memory, routing
everything through the handler and `IMessage`, is the only way to test what
`kDistributable` actually promises.

### CLAP — `tinyplug_tests_clap`

Post-headless, the easiest of all: `clap_plugin` is a single object reached
through a plain C vtable. Build a `clap_host` struct with test callbacks
(`params.rescan`, `params.request_flush`, `latency.changed`, `state.mark_dirty`,
`gui.request_resize`, `thread_check`) and drive `clap_process` directly. Add
`clap_input_events`/`clap_output_events` fakes. `thread_check` returning the
*wrong* answer is a useful hostile-host case.

### AAX — `tinyplug_tests_aax_alg` (now) + `tinyplug_tests_aax_model` (post-headless)

The two-component split is a testing gift: the algorithm's only window on the
world is `Alg_context`, a struct of pointers, and `alg_proc.cpp` needs nothing but
AAX headers. Build an `Alg_context` by hand, place `Alg_state` via `alg_init`,
push `Coef_segment`s, call `alg_render_stereo`, read the output and the return
`Byte_ring`. That directly tests:

- Segment `seq` skipping; the shadow diff producing exactly the changed
  addresses; NaN-seeded shadow emitting everything on first delivery.
- The bypass pseudo-param at `bypass_address`.
- Direct Data staging: meters, worker messages, latency proposals into the ring;
  `ReadPortDirect` semantics; **the ~30 ms non-guaranteed wakeup** — assert
  nothing in the pipeline assumes a rate, by driving the drain at wildly irregular
  intervals and with long gaps that overflow the ring.
- The `latency_seq` gate (§5.6).

The data model side (`parameters.cpp`, chunks, taper delegates, `tree_to_aax_ids`)
follows with headless; `AAX_CEffectParameters` can be driven directly since the
host contact points are all virtual methods. `adapters.hpp` (`tree_to_aax_ids` /
`aax_id_to_tiny`) and the taper delegates are pure and can drop to T0 immediately.

### AUv2 — `tinyplug_tests_auv2`

Post-headless. `ausdk::AUBase` subclasses are instantiable directly in a test
binary on macOS; drive `Initialize`, `SetParameter`, `ScheduleParameter`,
`Render` (with a hand-built `AudioBufferList` and `AudioTimeStamp`),
`SaveState`/`RestoreState`, `GetParameterList`, `GetParameterInfo`.

The AUv2-specific prize is **`GetParameterList` == `au_order()`**, asserted
against the checked-in manifest (§8) — the surface Logic addresses by index.

### AUv3 — validator-only, plus a component-level smoke test

The hardest: the AU lives in an app extension, the view controller is ObjC, and
the extension is loaded out-of-process by the host. Two realistic options:

1. **In-process instantiation** — on macOS, register the `AUAudioUnit` subclass
   in a test binary and instantiate it directly (`initWithComponentDescription:`),
   bypassing the extension mechanism. Reaches `DSPKernel`, parameter tree,
   `fullState`, `internalRenderBlock` — most of §5 — but *not* the extension
   packaging, entitlements, or the container app.
2. **`auval` on the installed component** (T3) for the packaging half.

Do both, and accept that AUv3 has the thinnest T2 coverage. Note the wrapper has
its own `DSPKernel` sharing no code with AUv2, so its coverage genuinely cannot be
inherited.

### Cross-format conformance runner

The most valuable single artifact in T2: **one scenario list, five wrappers.** A
`Conformance_scenario` is a fixture + an automation lane + a state-load script +
expectations expressed in framework terms (event sequence, param trajectory,
undo depth, reported latency). Each format's harness supplies an adapter that
knows how to feed a scenario through its native API and read the trace back.

Then "does AAX behave like VST3?" is a diff of two traces rather than a paragraph
of prose in `CLAUDE.md` — and the documented divergences (`Block_start`,
`Immediate_only`) become explicit, named expected-differences instead of
surprises. This is also where `add_expected_failure` earns its place: the
`all_plugins` harness already carries a legitimate XFAIL for exactly this
(block-start delivery cannot converge with sample-accurate delivery without
wrapper-side chunking), and that shape recurs here.

---

## 7. Golden traces

Property assertions ("monotone", "lands on target", "block-size independent") are
true of many curves. A golden trace pins the one curve today's code draws, so a
change shows up as a diff rather than silence.

Adopt the `all_plugins` conventions wholesale, since they're proven:

- A handful of `(fixture, lane, config)` cases, blessed to `.f32` under
  `tests/golden/data/`, committed.
- **Three assertions per case, in order**: renders identically twice (a
  non-reproducible render can't be blessed); produces finite, non-silent output (a
  golden file of zeros passes forever and protects nothing); matches the golden.
- ULP-tolerant comparison (4 ULPs for traces; `1e-6` absolute or `1e-5` relative
  for audio), not bit-exact — blessed files are one binary shared across whatever
  machine produced them.
- `--bless` is the **only** way to write them and nothing invokes it
  automatically.

Per-format golden traces are where cross-format conformance becomes concrete: the
same lane rendered through all five wrappers should produce the same `.f32`, and
where it can't, the difference is a named, blessed exception.

---

## 8. Permanence manifests

Three permanence surfaces have silent failure modes and no build-time enforcement
today ([param-identity-and-ordering.md](param-identity-and-ordering.md)).
`validate_tree` checks a single build for internal consistency; every failure mode
that matters is a question about **change over time**.

A checked-in manifest per fixture *and per demo plug-in* — generated by a small
tool that links the real model — closes most of that gap at a fraction of
[param-lockfile.md](param-lockfile.md)'s cost:

```json
{ "params": [ { "address": 0, "raw": 0, "identifier": "gain",
                "keypath": "output.gain", "semantic": "Real",
                "policy": "automation", "steps": 2048 } ],
  "au_order": [0, 1, 2],
  "aax_ids":  { "0": "gain" },
  "clap_modules": { "0": "Output" },
  "vst3_ids": { "0": 0 } }
```

A test regenerates it and diffs. Any reorder, rename, removal, or `au_order`
insertion fails with a message naming the rule broken. Regeneration is explicit
(`--bless`), so the diff lands in review where a human reads it.

This subsumes the *checking* half of `param-lockfile.md`; what that plan still
adds afterwards is the client-facing story (a `params.lock` the plug-in author
owns) rather than the mechanism.

---

## 9. Validators (T3)

`tools/validate.py <build-dir> [--format …] [--plugin …]`, driving:

| Format | Tool | Invocation | Notes |
|---|---|---|---|
| AUv2/AUv3 | `auval` | `auval -v aufx <subtype> <mfr>` | Requires install to `~/Library/Audio/Plug-Ins`. Parse the summary line; capture full log. |
| VST3 | `pluginval` (`/Applications/pluginval.app`) | `--validate-in-process --strictness-level 10` | Level 10 includes the fuzzing/param-stress passes. Also run the SDK's own `validator` binary from `tiny::vst3sdk`. |
| CLAP | `clap-validator` (`../clap-validator/binaries`) | `validate <bundle>` | JSON output; parse per-test status. |
| AAX | `aax-validator` (`../aax-validator`) | `dsh` → `load_dish aaxval` → `runtests <path>` | Needs PACE running. **Strip quarantine from the whole validator tree first** (`find … -xattrname com.apple.quarantine -s`), or `load_dish` fails with OSErr −23. |

Three things make this useful rather than merely present:

1. **An expected-results manifest** (`tests/validators/expected.json`) recording
   known, triaged failures with a reason — e.g. `test.page_table.load` fails on
   every demo because the demos ship no page table, which is correct and should
   not be re-triaged monthly. Anything not in the manifest fails the run.
2. **Log capture** to `tests/validators/logs/<plugin>-<format>.log`, gitignored,
   so a CI failure is diagnosable without a local repro.
3. **A page-table-bearing demo**, so the AAX page-table tests actually exercise
   something — the open follow-up from
   [aax-validation-checklist.md](aax-validation-checklist.md) §1.

Validators are necessary and insufficient, and the plan should say why in one
place: they check *format compliance*, not *framework promises*. `auval` will not
tell you a ramp landed a sample late; `pluginval` will not tell you a flushed
parameter was delivered but never manifested; `clap-validator` will not tell you
an undo step coalesced wrongly. That gap is precisely Tier 2.

---

## 10. CI

`.github/workflows/tests.yml`, the natural companion to the CI item already in
[refactor-ideas.md](refactor-ideas.md).

| Job | Runner | Runs |
|---|---|---|
| `core` | macos-latest | T0 + T1, plus an ASan/UBSan build of both. Minutes. |
| `wrappers` | macos-latest | T2 for VST3, CLAP, AAX, AUv2 (+ AUv3 in-process). ASan on a schedule rather than per-PR if it's slow. |
| `wrappers-win` | windows-latest | T0, T1, T2 for VST3, CLAP, AAX. The path that's painful to verify locally, and where the last two Windows bugs were found (`cc9828f`). |
| `validators` | macos-latest, self-hosted if PACE is needed | T3. AAX may have to stay local-only if the runner can't host PACE — say so explicitly rather than silently skipping. |
| `demos` | macos + windows | The existing build matrix: all formats build, all demos build, Xcode preset for AUv3/iOS. |
| `permanence` | any | §8 manifest diff. Seconds, and it's the one that protects shipped sessions. |

TSan gets its own scheduled job — it's slow and needs the multi-threaded
scenarios (worker + editor + audio) rather than the whole suite.

---

## 11. Layout

```
tests/
  CMakeLists.txt              # enable_testing(); tiny_add_test_target() helper
  tests.cpp                   # main() — runs the audio_bench registry
  fixtures/
    probe/                    # models/, processor.hpp, (editor.hpp for the editor tier)
    settling_probe/
    full_probe/               # worker + latency + sidechain + editor
    bare_probe/               # nothing optional present
  harness/
    reference_host.hpp        # T1: the wrapper process loop, de-quirked
    automation.hpp            # lanes in absolute timeline samples
    trace.hpp                 # audio + meters + latency proposals + event log
    scenario.hpp              # Conformance_scenario, shared by every T2 target
    rt_check.hpp              # allocation/lock trap armed around process()
  core/                       # T0
    test_value_helper.cpp  test_params_tree.cpp  test_state_adapter.cpp
    test_undo_history.cpp  test_queues.cpp       test_formatter.cpp
    test_byte_ring.cpp     test_dsp.cpp
  contract/                   # T1
    test_delivery.cpp  test_ramps.cpp  test_latency.cpp  test_meters.cpp
    test_worker.cpp    test_view_loop.cpp  test_bypass.cpp
  wrappers/                   # T2 — one dir per format
    vst3/  clap/  auv2/  auv3/  aax/
    conformance/              # the shared scenario list + per-format adapters
  golden/
    cases.hpp  bless.cpp  data/*.f32
  permanence/
    manifests/*.json  bless.cpp
  validators/
    validate.py  expected.json  logs/   (gitignored)
```

Build knobs mirroring the proven `all_plugins` ones: `TINY_BUILD_TESTS` (default
ON when top-level, OFF when consumed as a subproject), `TINY_TESTS_SANITIZE`
(ASan+UBSan, tests only — never the demos), and test targets that opt out of the
demos' universal-binary and macOS-11 deployment constraints while keeping the
same warning set.

---

## 12. Phasing

| Phase | Content | Unblocks / value |
|---|---|---|
| **1** | `tests/` skeleton, `audio_bench` wired in, `enable_testing()`, T0 for `Value_helper` + params tree + `State_adapter` + `Undo_history` + `Byte_ring` | Immediate: the "one logic-dense untested piece" gets covered, and there's somewhere to put a regression test |
| **2** | T1 harness ported from `all_plugins` (`Reference_host`, lanes, `Trace`), probe + settling fixtures, delivery/ramp/latency/meter contract tests, golden traces | The contract exists in executable form |
| **3** | T2 VST3 (move `test_vst3_flush.cpp` here, generalize `Fake_vst3_host` to two components) + T2 AAX algorithm (`Alg_context` by hand) | The two hardest-to-reason-about wrappers get a net, with zero prerequisites |
| **4** | §8 permanence manifests + the `permanence` CI job | Protects shipped sessions; cheapest high-value item in the plan |
| **5** | T3 `validate.py` + expected-results manifest + log capture; page-table demo | One command replaces four bespoke procedures |
| **6** | CI: core + wrappers + demos + permanence, macOS and Windows | The multi-platform matrix stops being hand-checked |
| **7** | *(after [headless-plugin.md](headless-plugin.md))* T2 CLAP, AUv2, AAX data model | Full wrapper coverage |
| **8** | Null platform backend → editor/action/undo tier (§5.9); AUv3 in-process smoke test | Closes the last untested seam |
| **9** | RT-safety trap + TSan scheduled job; the cross-format conformance runner over the full scenario list | The invariants become cross-checked rather than independently asserted |

Phases 1–4 need nothing that doesn't exist today. Phase 7 is the only hard
dependency, and it's on work that's already planned for its own reasons.

---

## 13. Decisions to make

1. **Headless first, or stub the editor for tests?** Recommendation: headless
   first. It's already planned, it's the honest configuration, and a test-only
   stub would be a second way to build the wrappers that nobody ships.
2. **Vendor `audio_bench` or `FetchContent` the sibling?** Recommendation:
   `FetchContent` with a pinned tag, defaulting to `../audio_bench` the way
   `TINY_DEPS_PATH` defaults to `../tiny_deps` — consistent with existing
   practice, and CI can fetch.
3. **Fixtures under `tests/fixtures/` or promoted `examples/`?** Recommendation:
   `tests/fixtures/`. `automation_tester` stays a demo (it's useful in a DAW); the
   probe is a copy that's free to grow test-shaped params without cluttering the
   examples CI builds.
4. **Do golden traces live per-format, or only at T1?** Recommendation: T1 first;
   add per-format golden traces only once the conformance runner exists, since
   that's what makes a cross-format diff meaningful.
5. **AAX validator in CI?** Needs PACE on the runner. If that's not viable, make
   it an explicit local-only target with a documented cadence rather than a
   silently-skipped job.
6. **Where does `all_plugins`' harness end up?** Recommendation: the *framework*
   half (`Fake_host`, lanes, trace, the VST3 flush suite) moves here and
   `all_plugins` consumes it; the product half (operating points, golden audio,
   product descriptors) stays there. Worth confirming before duplicating code.

---

## 14. Key files

| File | Role |
|---|---|
| [examples/automation_tester/](../examples/automation_tester/) | Source of the probe fixture |
| [wrappers/vst3/source/audio_effect.cpp](../wrappers/vst3/source/audio_effect.cpp) | The reference process loop T1 is modelled on; T2's first target |
| [wrappers/aax/source/alg_proc.cpp](../wrappers/aax/source/alg_proc.cpp) / [alg_context.hpp](../wrappers/aax/source/alg_context.hpp) | Editor-free, directly drivable — T2 AAX algorithm |
| [wrappers/aax/source/byte_ring.hpp](../wrappers/aax/source/byte_ring.hpp) | Pure, T0-testable, and the basis of Direct Data's lock-freedom |
| [libs/tinyplug/source/value_helper.cpp](../libs/tinyplug/source/value_helper.cpp) | Densest untested logic |
| [libs/tinyplug/include/tinyplug/tiny_view.hpp](../libs/tinyplug/include/tinyplug/tiny_view.hpp) | `run_frame` — the canonical UI loop, §5.9 |
| [plans/headless-plugin.md](headless-plugin.md) | Prerequisite for T2 on CLAP / AUv2 / AAX data model |
| [plans/param-lockfile.md](param-lockfile.md) | §8 delivers its checking half |
| [plans/aax-validation-checklist.md](aax-validation-checklist.md) | T3 AAX procedure and known-good baseline |
| `all_plugins/tests/` | Harness + VST3 flush suite to port |
