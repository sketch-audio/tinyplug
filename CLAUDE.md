# CLAUDE.md

Operational notes for agents working on tinyplug. This file is descriptive,
not normative — when the code disagrees with it, trust the code and update
this file.

## What this project is

tinyplug is a C++20 audio plug-in framework that wraps a single user
processor/editor pair into AAX, AUv2, AUv3, CLAP, and VST3 binaries. The
user writes format-agnostic code; per-format wrappers under [formats/](formats/)
translate the host's API into framework events and back.

- Repo layout. All three libraries are peers under [libs/](libs/), each with its own `CMakeLists.txt`
  and an isolated `include/<name>/` PUBLIC root (you only see a lib's headers if you link it):
  - [libs/tinyplug/](libs/tinyplug/) — core framework. Public headers in
    `include/tinyplug/` (`<tinyplug/...>`, umbrella `<tinyplug/tinyplug.hpp>`), impls
    in `source/`. OS-detection macros live in `<tinyplug/platform_defs.hpp>`.
  - [libs/tiny_platform/](libs/tiny_platform/) — native window/dialogs/paths/Skia
    (macOS/iOS/Windows). Headers `<tiny_platform/...>`, sources in `source/`, config
    templates in `cmake/`. Static lib; links core PUBLIC, Skia PRIVATE.
  - [libs/tiny_dsp/](libs/tiny_dsp/) — header-only DSP helpers (`Host_bypass`,
    `Linear_ramper`, `Delay_line`), `<tiny_dsp/...>`. INTERFACE lib, pure leaf.
  - [wrappers/](wrappers/) — one wrapper per format (was `formats/`).
  - [examples/](examples/) — demo plug-ins consumed by CI (was `plugins/`).
  - [cmake/](cmake/) — `helpers.cmake`, `plug_info.hpp.in`, etc.
  - [template/](template/) + [tools/new_plugin.py](tools/new_plugin.py) — scaffold
    a new plug-in (a simple gain effect, no worker): `python3 tools/new_plugin.py
    "My Plug" --manu Acme --id plg1`. Generates `examples/<snake_name>/` and appends
    it to [examples/CMakeLists.txt](examples/CMakeLists.txt).
  - [tools/](tools/) — author helper utilities: the scaffold above, preset exporters
    ([tools/presets/](tools/presets/)), and AAX page-table generation
    ([tools/pagetables/](tools/pagetables/)). All opt-in; demos don't use them.
  - [plans/](plans/) — design docs for in-flight work.

## Build

External SDKs live in a sibling [tiny_deps](../tiny_deps) repo. The simplest path
is the CMake presets (they default `TINY_DEPS_PATH` to `../tiny_deps` and
`TINY_INSTALL_PLUGINS=OFF`):

```
cmake --preset debug && cmake --build --preset debug   # Makefiles, all formats except AUv3
cmake --preset macos && cmake --build --preset macos   # Xcode, incl. AUv3
```

Presets: `debug` (Unix Makefiles → `build-debug`), `macos` / `ios` (Xcode →
`build-macos` / `build-ios`, required for AUv3), `windows` (VS, Windows hosts only).
The manual equivalents still work, e.g.:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTINY_DEPS_PATH=../tiny_deps
cmake --build build
```

- `--parallel` is allowed (e.g. `cmake --build build-debug --parallel 8`), but
  never launch a second build while one is already running. The `debug` build
  preset already sets `jobs: 8`.
- macOS Xcode generator is required for AUv3 (`--preset macos`).
- iOS AUv3: `--preset ios`.
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
- **`params::Model`** ([libs/tinyplug/include/tinyplug/tiny_params.hpp](libs/tinyplug/include/tinyplug/tiny_params.hpp)) —
  enumerates `Address` and provides `build_tree()` returning a
  `params::Node` tree (groups + specs). The framework flattens the tree to an
  indexable array but preserves structure where the format supports it
  (AUv2 clumps, VST3 units, AAX page tables, CLAP modules, AUv3 parameter
  groups). A model may additionally satisfy `params::Au_ordered` by declaring
  `au_order() -> std::vector<Address>` — see "Parameter permanence" below.
- **`Meter_model`** ([shared/tinyplug/tiny_meters.h](shared/tinyplug/tiny_meters.h)) —
  same shape as params, but with `Meter_policy::{peak,stream,trig}` for how
  the editor consumes updates.
- **`Plug_worker`** ([shared/tinyplug/tiny_worker.h](shared/tinyplug/tiny_worker.h)) —
  optional. If the plug-in source dir contains `plug_worker.h` it is
  discovered via `__has_include` and `TINY_HAS_WORKER` is defined. Otherwise
  `No_worker` (monostate) collapses every worker member to nothing.

## Parameter permanence

A parameter model has **three** independent permanence surfaces, not one. Design
notes and the sourcing behind each: [plans/param-identity-and-ordering.md](plans/param-identity-and-ordering.md).

- **`Identity::address`** — the persistence and automation key in every format
  (VST3 `ParamID`, CLAP `id`, AUv2/AUv3 address, and the AAX string ID is derived
  from it). Assign at the end of the `Address` enum. Never change, reuse, or
  remove — retire a parameter with `Policy::Hidden` and it keeps its slot.
- **`Identity::identifier`** and `Group::identifier` — the AUv3 `keyPath` is the
  dot-joined chain of ancestor group identifiers plus the parameter's own, and
  preset JSON nests by exactly the same chain. So a parameter **may not move
  between groups**, and no identifier may be renamed. `validate_tree` enforces
  non-empty, unique-among-siblings, and globally-unique keypaths at startup.
  The root group is exempt — it contributes to no path.
- **`au_order()`** — the AUv2 parameter list. Logic addresses AUv2 automation by
  *index into this list*, not by id, so it must be append-only across releases.
  This is why it is declared separately from the tree: `build_tree()` also encodes
  display order, and inserting a parameter next to its visual siblings shifts
  every list position after it. With `au_order()` the tree stays free — put a new
  parameter wherever it looks right, then append it to `au_order()`.

`Param_order::Au_ordinal` returns that order and
[wrappers/auv2/source/effect.cpp](wrappers/auv2/source/effect.cpp) `GetParameterList`
is its only consumer. A model that doesn't declare `au_order()` falls back to
**address order** (`Indexable`), which is append-only by construction since the
`Address` enum is — it costs you control of the AU display order but is never
unsafe. It must never fall back to tree order: that would make the tree itself
append-only, which is exactly the constraint this design removes.
Everything else is free to change: `name`, `short_name`, `Group::name`, and the
whole display hierarchy provided ancestry is preserved. Group *names* (not
identifiers) are what CLAP modules, VST3 units and AUv2 clumps are built from, so
those are cosmetic.

## Three spaces, one parameter

Conversions are everywhere; the central type is `params::Value_helper` in
[value_helper.hpp](shared/tinyplug/value_helper.hpp) /
[value_helper.cpp](shared/tinyplug/value_helper.cpp) (the declarative model lives
in [tiny_params.hpp](shared/tinyplug/tiny_params.hpp)):

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
`Value_helper::knob_to_plain` vs `host_to_plain` etc. (Knob space and the old
"norm" space are the same thing; `plain_to_knob`/`knob_to_plain` replace the
former `plain_to_norm`/`norm_to_plain`.)

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

### AAX (wrappers/aax/) — TWO-COMPONENT, DECOUPLED

Like VST3, AAX is split — but along a *different* seam. VST3 splits
processor/controller; AAX splits **algorithm** (a stateless C callback owning
the DSP) from **data model** (`Parameters`, owning parameters, editor, worker,
undo, and state chunks). The GUI hangs off the data model. Design rationale and
the SDK evidence behind every choice: [plans/aax-two-component.md](plans/aax-two-component.md).

- **The algorithm's only window on the world is `Alg_context`**
  ([alg_context.hpp](wrappers/aax/source/alg_context.hpp)) — a struct of
  pointers the host repopulates before each call. There is deliberately no
  route back to the data model object. `AAX_eProperty_Constraint_Location` is
  **not** set, so co-location is not even assumed.
- **Arbitrary parameters ride in segmented coefficient packets.**
  `AAX_FIELD_INDEX` is `offsetof/sizeof(void*)` — a pointer-slot index — so the
  C array `Coef_segment* coefs[num_segments]` occupies contiguous, arithmetically
  derivable field indices; Describe and the algorithm agree via `coef_field()`.
  No code generation. 15 doubles + a `seq` = exactly 128 bytes, the HDX minimum
  transfer size Avid recommends targeting. **Values are PLAIN space** — the data
  model converts, off the real-time thread.
- **The algorithm diffs, the data model doesn't track.** A segment whose `seq`
  is unchanged is skipped; otherwise its ≤15 values are compared against
  `Alg_state::shadow` (seeded with NaN, so the first delivery emits everything)
  and each difference becomes a `Set_param`. Don't replace this with a dirty
  bitmask — the point is that the data model holds no per-address knowledge of
  what the algorithm has consumed.
- **The master bypass is a pseudo-parameter** at `bypass_address == num_params`,
  packed into the segments like any other value, so `Host_bypass` lives in
  `Alg_state` next to the kernel. The data model tracks no bypass state.
- **`Alg_state` (kernel + bypass + shadows) lives in a private data block**, and
  is placement-new'd by the `AAX_CInstanceInitProc` — **not** by `ResetFieldData`,
  whose block is copied into the algorithm's memory pool and would require
  `Alg_state` to be trivially relocatable. The init callback runs *after* packet
  delivery, so the `Config_packet`'s sample rate is available and the kernel's
  allocating `reset(sr)` happens off the real-time thread.
- **Everything flowing outwards uses Direct Data**
  ([direct_data.cpp](wrappers/aax/source/direct_data.cpp)): meters, worker
  messages and latency proposals are staged in a `Byte_ring`
  ([byte_ring.hpp](wrappers/aax/source/byte_ring.hpp)) inside private data and
  copied across by `ReadPortDirect` — a memcpy of a byte range, the AAX analogue
  of a VST3 `IMessage`. The wakeup is **~30 ms and not guaranteed regular**, so
  nothing may assume a rate. The producer never overwrites unread data (a full
  push drops), which is what lets the remote consumer read without a seqlock retry.
- Direct Data reaches the data model through `Get/SetCustomData` — the SDK's
  documented inter-module hook — never by casting `AAX_IEffectParameters*`.
- **Automation timing is host-managed.** Packets posted inside
  `GenerateCoefficients()` are timestamped with the breakpoint position, and the
  host splits render buffers down to 32 samples to land them. **Never post from
  anywhere else** — except the runtime packet from `TimerWakeup()`, which is safe
  only because that port is buffered (PTSW-187216).
- AAX is the only format where the manufacturer can ship both stereo and mono
  variants of one plug-in; [describe.cpp](wrappers/aax/source/describe.cpp) adds
  one algorithm component per stem format when `Plug_info::can_process_mono`,
  offsetting `plugin_id` by 1 for mono.
- **Every pointer slot in `Alg_context` must be registered** or the host corrupts
  the context; unused slots get a small `AddPrivateData` filler.
- **AAX validator quirk**: parameters with more than 2048 steps fail validation.
  [parameters.cpp](wrappers/aax/source/parameters.cpp) clamps to 2048 for every
  semantic, even `Real`/`Fixed`. There's a forum-link comment by every clamp.
- **State chunk** uses `State_rules::Aax::chunk_id = 'tiny'` with named string
  keys (`tinyplug-num-params`, `tinyplug-edit-keys`, `tinyplug-host-bypass`).
  Pure data model — untouched by the two-component split. `CompareActiveChunk`
  is required for Pro Tools' compare light; current implementation only compares
  params.
- AAX parameters are addressed by **string IDs**, not integers; `tree_to_aax_ids`
  in [adapters.hpp](wrappers/aax/source/adapters.hpp) builds canonical IDs from
  the param tree, and `aax_id_to_tiny` reverses the map.
- Custom taper delegates (`Real_semanticsTaperDelegate`, `Fixed_semanticsTaperDelegate`,
  [taper_delegate.hpp](wrappers/aax/source/taper_delegate.hpp)) exist so AAX's
  normalized-to-plain transform respects our `Knob_adapter`.
- Latency change protocol: kernel proposes → algorithm pushes onto the return
  ring → Direct Data calls `SetSignalLatency` → AAX delivers
  `AAX_eNotificationEvent_SignalLatencyChanged` → data model reads back what the
  host accepted (the host owns latency in AAX) → next `Runtime_packet` carries it
  with a bumped `latency_seq`, and the algorithm issues `Accepted_latency`.
  The **sequence, not the value**, gates application — a zero-initialised packet
  must not read as "the host accepted zero".

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
  the optional buffer-source persistence from
  [plans/buffer-system.md](plans/buffer-system.md) for large audio buffers
  (not yet implemented).
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

### Host-initiated preset/state loads → undo + `notify`

When the host loads a preset / full state, each format's restore entry point
(VST3 `setComponentState`+`setState`, CLAP `_update_state`, AUv2 `RestoreState`,
AUv3 `setFullState`, AAX `SetChunk`) snapshots all param values in **knob space**
before and after applying the load, then calls
`Undo_history::push_host_load(before, after, out_changes)` to record **one
coalesced undo step**. The `Undo_history` lives on the long-lived wrapper class
(`Controller`/`Plugin`/`Effect`/`Auv3_AUAudioUnit`/`Parameters`), **not** the
view — the view borrows it via a `Undo_history*` in its `Deps` — so a host load
is captured into undo **even when the editor window is closed**. The undo replay
path is unchanged (`apply<>` pushes `Action_start`/`Set_param`/`Action_end` back
through the editor's action handler to the host).

The load is then surfaced to the editor by the wrapper calling
`Editor::notify(Host_event{Host_preset_loaded{...}})` **synchronously** from the
restore path — *not* deferred to the view loop. Because the `Editor` also lives at
wrapper lifetime, this fires whether or not the GUI is open, **once per load**, so
multiple closed-editor loads each notify on their own undo step (no dropped
intermediates). The event carries `changes` (the diff), `params` (full post-load
values, knob space), and `add_param(addr, knob)`, which applies an editor-owned
marker param through the normal host/processor/UI path **and** folds it into the
same undo step via `amend_host_load` — so a preset's name/index marker undoes
together with its values. VST3 splits the load across two calls: the step is
pushed in `setComponentState` and the `notify` is dispatched at the end of
`setState` (guarded by a pending flag), where the editor state has also arrived
and the step is still open. `notify` also delivers window events
(`Dark_mode_changed`) — it is the editor's single notification entry point
([tiny_events.hpp](libs/tinyplug/include/tinyplug/tiny_events.hpp):
`Host_event = variant<Host_preset_loaded, Dark_mode_changed>`). Limitations:
**params only** (not the editor `State_map`); a host single-param edit /
automation is **not** surfaced (only full-state loads — source attribution is a
planned follow-up); `notify` may arrive with the window closed, so authors must
only mutate editor state in it, not touch live view resources. Assumes all
restore entry points run on the UI/main thread (they do on all five hosts).

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
and the Skia surface. Selection is `#if TINY_PLATFORM_MACOS / TINY_PLATFORM_IOS /
TINY_PLATFORM_WINDOWS`; selection at compile time only.

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

- **The structural + naming refactor has landed** (`next` branch): the `libs/`
  layout (`tinyplug` core + `tiny_platform` + `tiny_dsp`, each with an isolated
  `include/<name>/` root), Skia `PRIVATE`, the `params`/`meters`/`models`/`plugin`
  namespaces, `CMakePresets`, the worker `Model` restructure, and the `tools/`
  consolidation. What remains is an optional backlog —
  **[refactor-ideas.md](plans/refactor-ideas.md)** (CI, clang-format, PCH/unity,
  further namespace passes, `detail/` split, downstream migration).

- **[param-lockfile.md](plans/param-lockfile.md)** — deferred. A checked-in
  `params.lock` per plug-in plus a `<Plugin>_paramlock` tool that links the real
  model, so the permanence rules above become build-time enforcement instead of
  convention. `validate_tree` can only check a single build for internal
  consistency; every failure mode that matters is a question about change over
  time. Design is settled; four open decisions are listed in the doc.

- **[midi-support.md](plans/midi-support.md)** — adds note/MIDI types
  to the `Render_event` variant (`Note_on`, `Note_off`, `Note_choke`,
  `Note_expression_value`, `Midi_cc`, `Pitch_bend`, `Channel_pressure`)
  and a separate `Midi_output_event` variant for outbound MIDI. Velocity/CC
  values are normalized doubles; `Note_id` unifies VST3 noteId / CLAP
  note_id / AU+AAX channel+key. This is the gateway to instrument
  plug-ins (synths) and MIDI effects.

- **[block-output.md](plans/block-output.md)** — outbound vector transport,
  processor→editor. `Block_model` for scopes/FFTs and the waveform overviews
  the buffer system draws with, with `snapshot` (triple-buffer) and `stream`
  (FIFO) policies. Same declarative shape as params/meters; value-semantics
  across the VST3 COM boundary.

- **[buffer-system.md](plans/buffer-system.md)** — managed large audio buffers
  (looper / granular / sampler / drum machine). One opt-in declarative
  `Buffer_model`: the processor owns a canonical `Buffer_source` (persisted in
  the session), off-thread `prepare_buffer` derives an RT-ready `Prepared`,
  installed via atomic pointer-swap + deferred retire. Every editor↔processor
  leg is value semantics; pointer-swap is intra-processor only. Unifies and
  supersedes the former `state-model`, `asset-store`, and the Table half of
  `block-table-io`. Backward-compatible (empty model = no overhead).

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
- **Don't reorder or remove `Address` values** once a plug-in has
  shipped — `enum_raw(addr)` is the persistence key. Adding new values
  at the end is fine.
- **Don't reorder `au_order()`, and don't rename an `identifier`.** Both are
  permanence surfaces with silent failure modes — see "Parameter permanence".
  Appending to `au_order()` is fine; rearranging the *tree* is fine too, which
  is the whole reason the two are separate.
- **Never run two builds at once.** `--parallel 8` is fine
  (`cmake --build build-debug --parallel 8`); what's not fine is launching a
  second build while one is still running.
- **Worker reply handlers are concept-detected at compile time.** If a
  user's `Plug_processor` declares `handle_worker_reply(const To_processor&)`,
  it gets called automatically; if not, the drain is a no-op. The check
  has to live inside a template so `if constexpr` properly discards the
  un-detected branch — don't move it out.
