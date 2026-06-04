# AGENTS.md

Operational notes for agents working on tinyplug. This file is descriptive,
not normative — when the code disagrees with it, trust the code and update
this file.

## What this project is

tinyplug is a C++20 audio plug-in framework that wraps a single user
processor/editor pair into AAX, AUv2, AUv3, CLAP, and VST3 binaries. The
user writes format-agnostic code; per-format wrappers under [formats/](formats/)
translate the host's API into framework events and back.

- Repo layout (pre-refactor, see [plans/structural-and-naming-refactor.md](plans/structural-and-naming-refactor.md)):
  - [shared/tinyplug/](shared/tinyplug/) — core framework headers + impls
    (params, meters, events, view, worker, tasks, undo, state).
  - [shared/platform/](shared/platform/) — native window/dialogs/paths/Skia
    integration (macOS/iOS/Windows).
  - [shared/dsp/](shared/dsp/) — small header-only DSP helpers
    (`Host_bypass`, `Linear_ramper`, `Delay_line`).
  - [formats/](formats/) — one wrapper per format.
  - [plugins/](plugins/) — demo plug-ins consumed by CI.
  - [template/](template/) — `new_plugin.py` scaffold source.
  - [cmake/](cmake/) — `helpers.cmake`, `plug_info.h.in`, etc.
  - [plans/](plans/) — design docs for in-flight work.

## Build

External SDKs live in a sibling [tiny_deps](../tiny_deps) repo. The path is
required:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTINY_DEPS_PATH=../tiny_deps
cmake --build build
```

- **Always build serially.** Never pass `--parallel` and never launch a
  second build while one is running — the codebase has a memory recorded
  enforcing this.
- macOS Xcode generator is required for AUv3:
  `cmake -S . -B build-macos -G Xcode -DTINY_DEPS_PATH=../tiny_deps`.
- iOS AUv3: `cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS`.
- `TINY_BUILD_PLUGINS=ON` (default) builds the demos; `TINY_INSTALL_PLUGINS=ON`
  copies bundles to `~/Library/Audio/Plug-Ins/...` (or the Windows
  equivalent) after each build.

## Core abstractions

The plug-in author implements two classes plus a few static models. Concepts
live alongside each interface — find them by searching for `concept Some_*`.

- **`Plug_processor`** ([shared/tinyplug/tiny_processor.h](shared/tinyplug/tiny_processor.h)) —
  `reset(sr)`, `handle_event(Render_event)`, `process(Dsp_context&)`,
  `latency_samps()`, `tail_samps()`. The concept is `Some_plug_processor`.
  Events are interleaved with `process` calls by the wrapper so DSP code
  always sees them at the right sample offset (sample-accurate automation
  including ramps).
- **`Plug_editor`** — receives `Plugin_state` (read-only params + meters)
  on draw, emits `User_action` events (Action_start/Set_param/Action_end/
  Request_resize). Wrappers translate gestures into host-native begin/edit/end
  notifications. Editor never shares memory with the processor.
- **`Param_model`** ([shared/tinyplug/tiny_params.h](shared/tinyplug/tiny_params.h)) —
  enumerates `Param_address` and provides `build_tree()` returning a
  `Param_node` tree (groups + specs). The framework flattens the tree to an
  indexable array but preserves structure where the format supports it
  (AUv2 clumps, VST3 units, AAX page tables, CLAP modules, AUv3 parameter
  groups).
- **`Meter_model`** ([shared/tinyplug/tiny_meters.h](shared/tinyplug/tiny_meters.h)) —
  same shape as params, but with `Meter_policy::{peak,stream,trig}` for how
  the editor consumes updates.
- **`Plug_worker`** ([shared/tinyplug/tiny_worker.h](shared/tinyplug/tiny_worker.h)) —
  optional. If the plug-in source dir contains `plug_worker.h` it is
  discovered via `__has_include` and `TINY_HAS_WORKER` is defined. Otherwise
  `No_worker` (monostate) collapses every worker member to nothing.

## Three spaces, one parameter

Conversions are everywhere; the central type is `Value_conv` in
[tiny_params.h](shared/tinyplug/tiny_params.h):

| Semantics | Plain space            | Host space         | Knob space |
|-----------|------------------------|--------------------|------------|
| Bool      | 0…1                    | 0…1                | 0…1        |
| List      | 0…(size-1)             | 0…(size-1)         | 0…1        |
| Int       | min…max                | min…max            | 0…1        |
| Fixed     | min…max                | min…max            | 0…1        |
| Real      | min…max                | 0…1                | 0…1        |

The DSP kernel sees **plain** values. The host sees **host** values. The UI
draws in **knob** (always 0…1) values. Wrappers shuttle between them — a
common bug shape is converting in the wrong space, so always check
`Value_conv::knob_to_plain` vs `host_to_plain` etc.

`Real_semantics` carries a `Knob_adapter` variant (`Adapt_lin`, `Adapt_log`,
`Adapt_pow`, `Adapt_taper`, `Adapt_piece`) controlling the knob↔plain
mapping. Conversions through these adapters have asserts on their domains —
log requires `min_val > 0`, taper requires `0 < taper < 1`, etc.

`Host_policy` (automation/control/hidden/interface) fully enumerates how a
parameter should be exposed and persisted — never flags-and-bits.
`State_rules::is_persistent` is the canonical predicate.

## Thread model and message queues

Communication uses `Lock_free_queue<T, capacity, Queue_concurrency::*>`
([shared/tinyplug/lock_free_queue.hpp](shared/tinyplug/lock_free_queue.hpp),
a detemplated farbot port). Common topology:

- **Host → processor**: format-native event stream, normalized into
  `std::vector<Tagged_event>` per render (with offsets and ramps).
- **Editor → host → processor**: `User_action` from the editor goes via
  the host's gesture/edit API; the processor receives the resulting
  parameter changes alongside automation.
- **Processor → editor**: meter updates pushed onto a `Set_meter` queue;
  the view drains in `run_frame` per `Meter_policy`.
- **State load → processor**: a `Lock_free_queue<Set_param>` (the
  "state queue") so chunk loads can publish into the audio thread without
  allocating.

The `view_impl::run_frame` template in [tiny_view.h](shared/tinyplug/tiny_view.h)
is the canonical UI loop: drain meters → call user's `on_gui_draw` → observe
actions for undo → dispatch actions → reset meter state. All wrappers route
through it; if you change frame semantics, change them here.

## Worker channel

`User_worker` runs on its own thread (`Worker_runner` in
[tiny_worker.h](shared/tinyplug/tiny_worker.h)), polled at
`User::poll_interval`. The four typed channels — `From_processor`,
`From_editor`, `To_processor`, `To_editor` — are independent variants of
trivially-copyable alternatives. Per-format wiring:

- **AAX / AUv2 / AUv3 / CLAP** — worker lives in-process, all queues are
  direct lock-free queues. The audio thread pushes into
  `_worker_from_proc`; the editor pushes into `_worker_from_edit`; replies
  drain into the processor on its next `process` call and into the editor
  in `run_frame`.
- **VST3** is the odd one (see next section).

If `plug_worker.h` is absent the user worker types collapse to
`std::monostate`, every worker queue member is `#if TINY_HAS_WORKER`-gated
out, and `if constexpr (has_worker)` skips runtime work. Reply handlers on
the user's processor/editor are detected by concept
(`Receives_worker_reply_to_processor` / `Receives_worker_reply_to_editor`)
inside `try_drain_worker_to_*` *templates* — they have to be templates so
`if constexpr` can discard the branch in a dependent context.

## Per-format quirks

### VST3 (formats/vst3/) — TWO-COMPONENT, DISTRIBUTABLE

VST3's defining quirk for this codebase: the processor (`Vst3_processor`,
[vst3_processor.h](formats/vst3/source/vst3_processor.h)) and the controller
(`Vst3_controller`, [vst3_controller.h](formats/vst3/source/vst3_controller.h))
are **distinct COM components** registered separately in
[vst3_entry.cpp](formats/vst3/source/vst3_entry.cpp) with class flag
`Steinberg::Vst::kDistributable`. They share no memory, may live in
different processes, and communicate only via the host through
`IMessage` / `IComponentHandler::restartComponent`. The controller hosts
the editor; the processor hosts the DSP. Two UIDs (`controller_uid`,
`processor_uid`) are generated from the manufacturer/plug-in codes plus
"ctrl"/"proc" suffixes in [helpers.cmake](cmake/helpers.cmake).

This has knock-on effects throughout the wrapper:

- **State is split.** `Vst3_processor::getState`/`setState` persists
  param values; `Vst3_controller::getState`/`setState` persists editor
  `State_map`; `Vst3_controller::setComponentState` re-receives the
  processor's values so the controller's mirror stays in sync. A 4-word
  header (framework_code, manufacturer_code, plugin_code, count) prefixes
  each chunk; assertions verify the framework/mfr/plugin words on load.
- **Latency changes hop through the host.** Kernel proposes via
  `Dsp_context::propose_latency`; processor stores it into an atomic
  `_pending_latency`, then bumps a hidden "latency" output parameter to
  force the controller to learn that something changed. The controller
  notices the latency-param change, calls
  `restartComponent(kLatencyChanged)`, the host calls `getLatencySamples()`
  and toggles `setActive`, at which point the processor reads the pending
  value back, writes `_accepted_latency`, and the next `process` issues
  `Accepted_latency` to the kernel. Don't simplify this — it's how the
  state machine survives `kDistributable`.
- **Meters travel as output parameter changes.** There is no direct
  processor→controller channel for streaming data, so meters are written
  into `data.outputParameterChanges` at the end of `process` using
  parameter IDs in the `export_param_offset` range. The controller's
  `setParamNormalized` recognizes the range and pushes the (denormalized)
  value into the editor's meter queue.
- **Worker has to cross the COM boundary.** Editor↔worker is direct
  (worker lives on the controller side). Processor↔worker must traverse
  `IMessage`: audio-thread pushes lock-free into `_worker_outbound`, an
  `Outbound_message_shuttle` thread drains it and calls
  `sendMessage` to the controller; replies arrive in `Vst3_controller::notify`,
  get pushed into `_worker_to_proc`, and the controller's worker runner
  has a `set_post_cycle` that ships them back to the processor via another
  `IMessage`. See [vst3_messaging.h](formats/vst3/source/vst3_messaging.h)
  for the typed payload encoding (`send_variant` + `reconstruct_variant`,
  alternatives must be trivially copyable).
- **Output parameter changes for state-load events** are required because
  the controller's view of param values comes via `setComponentState`,
  not via the processor.
- **`vst3::Message_router`** dispatches by string ID; both processor and
  controller install handlers and route `notify()` through it. New
  wrapper-level traffic should prefix IDs with `tiny/` to avoid host
  collisions.

### AAX (formats/aax/) — vendored monolith

- The wrapper extends `AAX_CMonolithicParameters`, but the SDK class has
  been **vendored** into [aax_monolith.h](formats/aax/source/aax_monolith.h) /
  [aax_monolith.cpp](formats/aax/source/aax_monolith.cpp) and edited
  (replaces `kSynchronizedParameterQueueSize` with a per-plug-in compile-
  time size from `Param_infos<Param_model>::num_params`, removes
  `kMaxAuxOutputStems` waste). Don't replace this with the SDK version
  without understanding why.
- AAX is the only format where the manufacturer can ship both stereo and
  mono variants of a single plug-in; [aax_describe.cpp](formats/aax/source/aax_describe.cpp)
  calls `StaticDescribe` twice when `Plug_info::can_process_mono` is true
  and offsets `plugin_id` by 1 for the mono build.
- **AAX validator quirk**: parameters with more than 2048 steps fail
  validation. [aax_parameters.cpp](formats/aax/source/aax_parameters.cpp)
  clamps to 2048 for every semantic, even `Real`/`Fixed`. There's a
  forum-link comment by every clamp.
- The Pro Tools master bypass parameter is registered with the well-known
  `cDefaultMasterBypassID` and is wired through `_bypass` (`Host_bypass`,
  [shared/dsp/host_bypass.hpp](shared/dsp/host_bypass.hpp)).
- **State chunk** uses `State_rules::Aax::chunk_id = 'tiny'` with named
  string keys (`tinyplug-num-params`, `tinyplug-edit-keys`,
  `tinyplug-host-bypass`). `Compare_active_chunk` is required for
  Pro Tools' compare light; current implementation only compares params.
- AAX parameters are addressed by **string IDs**, not integers; the
  `tree_to_aax_ids` helper in [aax_adapters.h](formats/aax/source/aax_adapters.h)
  builds canonical IDs from the param tree, and `aax_id_to_tiny` reverses
  the map on the audio thread.
- Custom taper delegates (`Real_semanticsTaperDelegate`, `Fixed_semanticsTaperDelegate`,
  [aax_taper_delegate.h](formats/aax/source/aax_taper_delegate.h)) exist
  so AAX's normalized-to-plain transform respects our `Knob_adapter`.
- Latency change protocol: kernel proposes → wrapper notifies host →
  AAX delivers `AAX_eNotificationEvent_SignalLatencyChanged` → wrapper
  reads back what the host accepted (the host owns latency in AAX) →
  `_accepted_latency` propagates to the kernel.

### AUv2 (formats/auv2/) — macOS desktop AU

- Built only on Apple non-iOS. Extends `ausdk::AUBase`. Despite being
  "desktop only", it shares no code with AUv3 — the AUv3 wrapper has its
  own `DSPKernel` ([formats/auv3/source/extension/DSPKernel.hpp](formats/auv3/source/extension/DSPKernel.hpp)).
- View factory class name must be **unique per plug-in** because AUv2
  hosts load multiple plug-ins into the same process; collisions are
  silent runtime breakage. The user sets `TINY_AUV2_VIEW_CLASS` to a
  unique name; the property is propagated into `Plug_info::Auv2::view_class`.
  [auv2_view_factory.mm](formats/auv2/source/auv2_view_factory.mm) declares
  the class via Objective-C macros.
- `Plug_info::version` is also encoded into the Objective-C class names
  for `mac_view.mm` (see `configure_mac_view` in [helpers.cmake](cmake/helpers.cmake)):
  multiple versions of the same plug-in loaded concurrently would
  otherwise collide on `MacView`.
- Parameter clumps come from the `Param_group` tree via `Clump_map`.
- `kAudioUnitProperty_UserPlugin = 64000` is the convention for plug-in-
  specific properties — see [plug_info.h.in](cmake/plug_info.h.in).
- Latency change: `_pending_latency` flag, then `PropertyChanged
  (kAudioUnitProperty_Latency)`, then on `GetLatency` we read the pending
  value and store it as accepted.

### AUv3 (formats/auv3/) — extension + container app (+ shared framework on iOS)

- AUv3 produces **two targets on macOS** (container app + AU extension)
  and **three on iOS** (container app + AU extension + a shared `*_core`
  framework that holds the actual AU/ViewController/view code). On iOS the
  extension is a literal one-symbol stub (`auv3_stub.m`); both the app and
  the extension link the shared framework and load
  `Auv3_AUAudioUnit` / `Auv3_AUViewController` via `NSClassFromString`, so
  the AU code exists once on disk and is shared between the host process
  and the in-process app preview. The framework target is gated by
  `if(CMAKE_SYSTEM_NAME STREQUAL "iOS")` in
  [make_auv3_plugin.cmake](formats/auv3/make_auv3_plugin.cmake) — on macOS
  the extension compiles the AU sources directly because each plug-in's
  ObjC class names are already disambiguated via `configure_mac_view`.
- Entitlements, Info.plist, and asset catalogues are templated under
  [formats/auv3/cmake/](formats/auv3/cmake/). The iOS framework has its
  own `Info-Core.plist` and bundle ID (`<base>.core`).
- iOS and macOS share the wrapper sources. `TINY_IOS_DEVICE_FAMILY`
  selects ipad/iphone/universal (`UIDeviceFamily` in the plist).
- Optional IAP / App-Group entitlements are templated for the container
  app via `TINY_APP_GROUP_ID`, `TINY_APP_IAP_PRODUCT_ID`, `TINY_APP_IAP_TRIAL_ID`.
- Param values cached locally in `_hostvalues` atomics (host space)
  because AUv3 sends per-param values and the controller view needs them
  fast.
- Presets: a generated `auv3_preset_list.h` enumerates factory presets;
  AUv3 hosts can also save **user presets**. When loaded, the editor
  state map gets an extra key `preset-name` (string) by convention — see
  the README "Presets" section.

### CLAP (formats/clap/) — single-component, modern

- The simplest wrapper. Single class `Clap_plugin`
  ([clap_plugin.h](formats/clap/source/clap_plugin.h)) extends
  `clap::helpers::Plugin`. Studio One is noted as misbehaving — `MisbehaviourHandler::Terminate`
  is enabled only in debug builds.
- Param "modules" (slash-separated path strings from `tree_to_clap_modules`)
  give CLAP its hierarchy.
- State and editor map are stored together in one stream with a 5-word
  header (`State_rules::Clap::Header`).
- Has a preset discovery factory ([clap_preset_discovery.h](formats/clap/source/clap_preset_discovery.h))
  so CLAP hosts can enumerate the plug-in's preset folder.
- `_hostvalues` atomics in host space mirror what the audio thread sees
  (similar to AUv3). `_from_flush` queue carries state-load events that
  arrive outside the process callback (CLAP's `paramsFlush`).
- Latency change goes through `_pending_latency` → on next `activate`,
  call `clap_host_latency::changed(host)` → host calls `latencyGet`,
  reads the new value, and we write `_accepted_latency` for the next
  process.

## State / preset model

- **Three persistence surfaces**: per-param scalar (host-managed),
  editor `State_map` (string→variant<bool,int32_t,double,string>), and
  the optional "extra" `State_model` from
  [plans/state-model.md](plans/state-model.md) (not yet implemented).
- `State_adapter` ([shared/tinyplug/state_adapter.hpp](shared/tinyplug/state_adapter.hpp))
  is the format-agnostic glue: a JSON document with `version`, `params`,
  and `editor` keys. The same adapter serves both bundle presets and the
  user-facing save/load.
- Per-format chunk layouts in [shared/tinyplug/state_rules.hpp](shared/tinyplug/state_rules.hpp).
  Each format embeds a (framework/manufacturer/plugin) sentinel; mismatch
  asserts.
- Presets are JSON files with a user-chosen extension (default `.json`);
  CMake (`copy_presets` in [helpers.cmake](cmake/helpers.cmake)) places
  them into bundle Resources for AAX/AUv2/AUv3/CLAP/VST3 macOS bundles, and
  the format-native preset directory at install time on macOS/Windows.
  AAX uses `.tfx` placed in the bundle; VST3 uses `.vstpreset` placed by
  the installer.

## Latency change protocol (consistent across formats)

Every wrapper implements the same pattern:

1. Kernel sets `Dsp_context::propose_latency = N` during `process`.
2. Wrapper stores `_pending_latency = N` (atomic, lock-free
   `optional<uint32_t>`).
3. Wrapper signals the host "latency may have changed" using the
   format-native mechanism — see each format above.
4. Host calls back; wrapper reads `_pending_latency`, stores
   `_accepted_latency = N`, reports `N` to the host.
5. Next `process`, wrapper sends `Accepted_latency{N}` to the kernel and
   the kernel must immediately match (assertion checked).
6. `Host_bypass::set_latency` is updated in lockstep so soft-bypass
   PDC compensation tracks.

If you find yourself adding a special-case latency flow, you're probably
fighting this protocol.

## Platform layer

[shared/platform/](shared/platform/) handles per-OS view, dialogs, paths,
and the Skia surface. Selection is `#if PLATFORM_MACOS / PLATFORM_IOS /
PLATFORM_WINDOWS`; selection at compile time only.

- macOS: Cocoa view + Metal-backed `SkSurface`; per-plug-in ObjC class
  names generated by `configure_mac_view` in [helpers.cmake](cmake/helpers.cmake)
  to avoid runtime ObjC class collisions when many plug-ins are loaded
  in one process.
- iOS: UIKit + Metal, AUv3 only.
- Windows: Win32 window + (currently) Skia CPU backend. GPU/D3D12 path
  exists in source but is disabled (`WIN_GRAPHICS_GPU` defaults to `0` in
  [shared/CMakeLists.txt](shared/CMakeLists.txt)) pending CPU/GPU
  sync work — see README roadmap.
- `Task_manager` ([shared/tinyplug/task_manager.hpp](shared/tinyplug/task_manager.hpp))
  exposes background / main-thread / serial-queue dispatch. The view is
  the only thing that calls `bind_main` + `run_main`; user code uses the
  `Actor`.

## CMake mechanics

- Per-plug-in build: an `add_library(<PLUGIN>_lib STATIC)` carrying the
  user code and a bag of `TINY_*` properties (see
  [plugins/gain_demo/CMakeLists.txt](plugins/gain_demo/CMakeLists.txt) as
  reference). `configure_plug_info` reads those properties and generates
  `plug_info.h`. Then `make_<format>_plugin(<TARGET>)` for each desired
  format.
- Plug-in codes: `TINY_MANUFACTURER_CODE` and `TINY_PLUGIN_CODE` are
  four-character codes embedded in AAX/AU/VST3 IDs. Manufacturer code
  needs at least one capital letter (AU convention).
- `mac_view.mm` is compiled once **per wrapper target** (not per plug-in)
  with a unique ObjC class name so multiple plug-ins coexist in a host
  process — see `configure_mac_view` in [helpers.cmake](cmake/helpers.cmake).

## Style

From the README, enforced informally:

- Herb Sutter "AAA"/left-to-right: most lines start with `const auto`;
  functions use trailing return types (`-> ReturnType`).
- Stroustrup naming with snake_case for multi-word names; types and
  scoped-enum members capitalized (`Param_spec`, `Host_policy::automation`).
- Opening brace on a new line only for function definitions.
- Private members prefixed with `_`.

## In-flight refactors / future direction

Read [plans/](plans/) before non-trivial changes — these are not
speculation, they're scheduled work.

- **[structural-and-naming-refactor.md](plans/structural-and-naming-refactor.md)**
  (active, `next` branch). Phase 1 moves to a Pitchfork layout
  (`include/`, `src/`, `wrappers/`, `examples/`, `libs/platform/`),
  spins `tiny_platform` out of `tiny_shared_lib` (renaming the latter
  to `tiny_core`), demotes Skia from `PUBLIC` to `PRIVATE` on the core
  library, and adds CMakePresets / clang-format / PCH / unity builds /
  CI. Phase 2 is a flat-`tiny::` → sub-namespace refactor
  (`tiny::params::Spec`, `tiny::events::Render`, `tiny::user::Processor`,
  etc.) with empty placeholder namespaces for the upcoming model PRs.
  No backwards-compatible aliases — single big-bang PR.

- **[midi-support.md](plans/midi-support.md)** — adds note/MIDI types
  to the `Render_event` variant (`Note_on`, `Note_off`, `Note_choke`,
  `Note_expression_value`, `Midi_cc`, `Pitch_bend`, `Channel_pressure`)
  and a separate `Midi_output_event` variant for outbound MIDI. Velocity/CC
  values are normalized doubles; `Note_id` unifies VST3 noteId / CLAP
  note_id / AU+AAX channel+key. This is the gateway to instrument
  plug-ins (synths) and MIDI effects.

- **[state-model.md](plans/state-model.md)** — third persistence
  surface for large/binary state (audio loops, IRs, MIDI sequences).
  Declarative `State_model` mirroring `Param_model`/`Meter_model`;
  optional `save_state_item` / `load_state_item` on the processor; state
  items delivered through the existing render-event queue. Backward-
  compatible (empty model = no overhead).

- **[block-table-io.md](plans/block-table-io.md)** — vector transport
  between editor and processor. `Block_model` (processor→editor, for
  scopes/FFTs) and `Table_model` (editor→processor, for wavetables/IRs),
  with `snapshot` (triple-buffer) and `stream` (FIFO) policies. Same
  declarative shape as params/meters.

Other roadmap items from README: synth support (depends on MIDI), Linux
(CLAP & VST3), LV2, Windows GPU graphics, multitouch on Windows, software
graphics backend on macOS, more demo plug-ins.

## Things to be careful about

- **Don't break the VST3 split-state contract.** `setComponentState` on
  the controller is *not* the same as `setState`. Param chunk lives on the
  processor; editor chunk lives on the controller; the controller mirror
  of param values is established only via `setComponentState`.
- **Don't break event ordering.** The `_events` vector inside each
  wrapper's `process` is reserved at a specific capacity calibrated from
  `num_params`. Asserts fire when it's full. Increase the capacity rather
  than allocating on the audio thread.
- **Don't introduce allocation on the audio thread.** Lock-free queues
  are sized at compile time; the kernel must keep `latency_samps()` and
  `tail_samps()` realtime-safe; the worker exists specifically for
  non-realtime work.
- **Don't reorder or remove `Param_address` values** once a plug-in has
  shipped — `enum_raw(addr)` is the persistence key. Adding new values
  at the end is fine.
- **Don't use `--parallel` with cmake build.** Recorded preference; see
  [.claude/memory](../.claude) memory file. Run one build at a time.
- **Worker reply handlers are concept-detected at compile time.** If a
  user's `Plug_processor` declares `handle_worker_reply(const To_processor&)`,
  it gets called automatically; if not, the drain is a no-op. The check
  has to live inside a template so `if constexpr` properly discards the
  un-detected branch — don't move it out.
