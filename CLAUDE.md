# CLAUDE.md

Operational notes for agents working on tinyplug. This file is descriptive,
not normative — when the code disagrees with it, trust the code and update
this file.

## What this project is

tinyplug is a C++20 audio plug-in framework that wraps a single user
processor/editor pair into AAX, AUv2, AUv3, CLAP, and VST3 binaries. The
user writes format-agnostic code; per-format wrappers under [formats/](formats/)
translate the host's API into framework events and back.

- Repo layout. The libraries are peers under [libs/](libs/), each with its own `CMakeLists.txt`
  and an isolated `include/<name>/` PUBLIC root (you only see a lib's headers if you link it):
  - [libs/tiny_core/](libs/tiny_core/) — the model-free half of the framework, `STATIC`,
    compiled once. Headers `<tiny_core/...>`, umbrella `<tiny_core/tiny_core.hpp>`, impls in
    `source/`. OS-detection macros live in `<tiny_core/platform_defs.hpp>`. **Nothing here may
    name a user model type** — see "Model layer".
  - [libs/tinyplug/](libs/tinyplug/) — the model-aware half, `INTERFACE` (header-only):
    events, actions, undo, processor, edit, view, worker. Umbrella `<tinyplug/tinyplug.hpp>`.
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

- **`Processor`** ([tiny_processor.hpp](libs/tinyplug/include/tinyplug/tiny_processor.hpp))
  — the whole process side lives in **`tiny::process`**, *including the user's class*, so
  a processor file writes the vocabulary unqualified:
  `configure(const Config&)`, `reset(const Reset::Any&)`, `handle(const Event::Any&)`,
  `process(Dsp_context&)`, `latency_samps()`, `tail_samps()`. The concept is
  `Some_plug_processor`.

  The user's classes live by **side**: `process::Processor`, `edit::Editor`,
  `work::Worker`. Framework vocabulary is mostly still in `tiny` and resolves unqualified
  from all three. Worker message types are part of the work model, so a processor reaching
  one writes `models::Tick`; see [worker_demo](examples/worker_demo/).
  Two axes, and the split is what to remember: **`configure` allocates** (sample rate
  plus the parameter values to come up holding, plain space) and is the only tier
  permitted to; **`reset` never does**. `Reset::Any` is a closed sum of block-boundary
  syncs — `Hard` (stream restarting, forget history, land everything), `Soft` (land what
  must be exact, history and long musical glides survive), `Latency{samples}` (the host
  accepted a proposal — adopt it now, `latency_samps()` must match on return). An
  exhaustive `std::visit` is the idiom, and it is what stops a kernel from silently
  ignoring an accepted latency.
  Events are interleaved with `process` calls by the wrapper so DSP code
  always sees them at the right sample offset (sample-accurate automation
  including ramps).
- **`Plug_editor`** — receives `Plugin_state` (read-only params + meters)
  on draw, emits `User_action` events (Action_start/Set_param/Action_end/
  Request_resize). Wrappers translate gestures into host-native begin/edit/end
  notifications. Editor never shares memory with the processor.
- **Process events and edit events are different types, and the difference is the value
  space.** `process::Event::{Set, Ramp}` carry **plain** values to the kernel;
  `Set_param` carries **knob** values from the editor / undo history. They used to be one
  type, which made a wrong-space assignment — the bug shape this codebase is most prone to
  (see "Three spaces") — compile silently. `Value_helper` is the bridge, at the wrapper
  boundary where it always was. `Change_set` is the one genuinely two-sided container and
  is templated on the event type so each instance declares its space.
  *Everything in `process::Event::Any` carries a frame offset*, which is what makes the
  `Tagged_event` sort meaningful. Anything the host says at a block boundary is a
  `Reset::Any`, not an event. Keep that line intact when adding MIDI.
- **`params::Model`** ([tiny_params.hpp](libs/tiny_core/include/tiny_core/tiny_params.hpp)) —
  enumerates `Address` and provides `build_tree()` returning a
  `params::Node` tree (groups + specs). The framework flattens the tree to an
  indexable array but preserves structure where the format supports it
  (AUv2 clumps, VST3 units, AAX page tables, CLAP modules, AUv3 parameter
  groups). A model may additionally satisfy `params::Au_ordered` by declaring
  `au_order() -> std::vector<Address>` — see "Parameter permanence" below.
- **`meters::Model`** ([tiny_meters.hpp](libs/tiny_core/include/tiny_core/tiny_meters.hpp)) —
  `num_meters` + `make_spec(uint32_t)`; addresses are `0..<num_meters`, and any enum
  naming them is the model's own business. `meters::Policy::{Stream,Peak,Trig}` sets how
  the editor consumes updates. The same header holds `Publisher` (processor side) and
  `Mailbox<M, Transport>`: `Framework` is lock-free for transports we own, while `Host`
  has no atomics and restates a dropped peak, for VST3, where the host delivers meters on
  the UI thread. Optional.
- **`blocks::Model`** ([tiny_blocks.hpp](libs/tiny_core/include/tiny_core/tiny_blocks.hpp)) —
  typed frames, processor → editor, latest wins. `Types` is a variant of trivially copyable
  frame types; `num_blocks` + a **constexpr** `make_spec(uint32_t)` whose `kind` picks each
  address's frame type at compile time. `Publisher` stages frames (processor writes through
  `Dsp_context::blocks`, a `Writer`), `Mailbox` is one lock-free `Data_port` per address,
  `Frames` is the editor's retained copy and `View` is what `Processor_state::blocks` hands
  it (`latest<A>()` / `fresh<A>()`). There is no `Host` mailbox for blocks: the VST3
  controller's `notify` can run on the relay's thread. Optional.
- **`state::Model`** ([tiny_state.hpp](libs/tiny_core/include/tiny_core/tiny_state.hpp)) — one
  trivially copyable struct both sides edit, synchronized and undoable; `writers`
  (`Editor`/`Processor`/`Both`) removes whatever the other side may not do. Every change is a
  `{base, next}` patch applied as a **byte merge** ([state_merge.hpp](libs/tiny_core/include/tiny_core/state_merge.hpp)),
  so a stale edit never erases a concurrent write. The processor side (`Processor_side`,
  `Store` in [state_store.hpp](libs/tiny_core/include/tiny_core/state_store.hpp)) applies a staged
  patch at the top of the block and triple-buffers what it publishes; `Dsp_context::state` is
  a per-block `Access`. The editor side is the wrapper-owned `state::Editor_link`
  ([tiny_state_link.hpp](libs/tinyplug/include/tinyplug/tiny_state_link.hpp)) — it holds the view,
  retains gestures to re-run on refusal, flushes once per frame (`Ui_receiver::sync_state`),
  and records document steps into the wrapper's `Undo_history` through `bind_state`. The
  editor sees it as `Edit_context::state`. **Persisted** as a record
  ([state_record.hpp](libs/tiny_core/include/tiny_core/state_record.hpp)): the author's
  optional `save`/`load` pair inside a framework container, appended after the bypass in the
  CLAP stream and the VST3 processor chunk, under `tinyplug-state` in AUv2/AUv3/AAX (base64 in
  AAX), and as `"state"` in preset JSON. A restore decodes it (default on anything refused),
  loads the processor copy (`on_session_load`) and calls `Editor_link::load`, which folds the
  change into the host-load undo step. AAX's algorithm doesn't see the load, so its data model
  loads with `Resend::Yes` and flushes. Design: [state-persistence.md](plans/state-persistence.md). Optional.
- **`work::Model`** ([tiny_work.hpp](libs/tiny_core/include/tiny_core/tiny_work.hpp)) — the
  worker's four channel variants plus tuning, declared as `models::Work` in
  `models/work.hpp`. Optional, and paired with `worker.hpp` (the `work::Worker` class).

## Model layer

A plug-in declares models in `source/models/{params,meters,blocks,state,work}.hpp` and classes in
`source/{processor,editor,worker}.hpp`. Discovery is **file presence, resolved by CMake**:
`configure_models()` / `configure_plugin()` ([helpers.cmake](cmake/helpers.cmake)) generate
`<tiny_models.hpp>` (`models::Resolved`, `User_params`/`User_meters`/`User_work`,
`TINY_HAS_*` + `has_*`) and `<tiny_plugin.hpp>` (`plugin::Resolved`,
`User_processor`/`User_editor`/`User_worker`) from
[tiny_models.hpp.in](cmake/tiny_models.hpp.in) / [tiny_plugin.hpp.in](cmake/tiny_plugin.hpp.in).
An absent model resolves to a zero-entry `<ns>::None`, never a monostate. Adding or removing a model file
re-globs on the next build (`CONFIGURE_DEPENDS`), except for single-target Xcode builds, which
skip `ZERO_CHECK`. Build `ZERO_CHECK` first there. Design:
[model-layer.md](plans/model-layer.md); client porting guide:
[MIGRATION.md](MIGRATION.md).

- **Why tinyplug is header-only.** A compiled-once library sees no models, so any TU there
  that included a model-aware header would compile a different layout than the plug-in — a
  silent ODR violation. Don't add a `.cpp` to `libs/tinyplug`; put model-free code in core.
- **Include direction.** Models include `<tiny_core/tiny_core.hpp>` only. `<tinyplug/tinyplug.hpp>`
  includes the generated `<tiny_models.hpp>` first, so user code never orders includes.
  `<tiny_plugin.hpp>` is for wrappers — it includes the user's own class headers.
- **Gate with both.** `#if TINY_HAS_*` removes members and fields; `if constexpr (has_*)`
  gates code, and only discards inside a template.
- **Meters are gated end to end.** Without `models/meters.hpp`, every wrapper compiles out its
  publisher, mailbox, transport (VST3 output params, AAX ring traffic) and the
  `Dsp_context::meters` / `Ui_receiver::read_meters` fields. `Processor_state::meters` stays,
  empty — it is core. A new meter path must sit behind `TINY_HAS_METERS` too.

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
- **Processor → editor**: `meters::Publisher` posts into a `meters::Mailbox`; the view
  reads one display value per meter in `run_frame`.
- **Latest value per parameter → processor or editor**: `Change_set`
  ([change_set.hpp](libs/tiny_core/include/tiny_core/change_set.hpp)), a double-buffered
  dense set with a dirty bitset. Fixed memory, can't overflow (writes coalesce), `push_n`
  lands whole, and the consumer never waits. Used for VST3 state loads, the VST3
  controller's host values, AUv2 editor edits and restores, AUv3 parameter-tree sets and
  CLAP `paramsFlush` (`Producers::One`: its producer can be the audio thread, so no mutex).
  Anything with a frame offset, a ramp or an order (automation, gestures) stays on a queue.

The `view_impl::run_frame` template in [tiny_view.hpp](libs/tinyplug/include/tinyplug/tiny_view.hpp)
is the canonical UI loop: read meters → call user's `on_gui_draw` → observe
actions for undo → dispatch actions. All wrappers route
through it; if you change frame semantics, change them here.

## Worker channel

`User_worker` runs on its own thread (`Worker_runner` in
[tiny_worker.hpp](libs/tinyplug/include/tinyplug/tiny_worker.hpp)), polled at
`User_work::update_period`. The four typed channels — `From_processor`,
`From_editor`, `To_processor`, `To_editor` — are independent variants of
trivially-copyable alternatives. Per-format wiring:

- **AAX / AUv2 / AUv3 / CLAP** — worker lives in-process, all queues are
  direct lock-free queues. The audio thread pushes into
  `_worker_from_proc`; the editor pushes into `_worker_from_edit`; replies
  drain into the processor on its next `process` call and into the editor
  in `run_frame`.
- **VST3** is the odd one (see next section).

If `worker.hpp` is absent the channel types collapse to `std::monostate`
(`work::None`), every worker queue member is `#if TINY_HAS_WORKER`-gated
out, and `if constexpr (has_work)` skips runtime work. Reply handlers on
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
- **Latency changes hop through the host, and the two paths are different.**
  A *configure-time* change (`setupProcessing`) is sent **directly** over
  `IMessage` (`tiny/latency/changed`) — no render is in flight there, and
  `_reported_latency.exchange` syncs the shadow whether or not the send
  lands, so a reconfigure can never be mistaken for a mid-render change.
  A *runtime* proposal (`Dsp_context::propose_latency`, raised on the audio
  thread) stores `_pending_latency` and posts a `tiny::Relay`, which sends
  the same message off the audio thread. This replaced a hidden "latency"
  output parameter that was bumped from `process`: Live mis-ingests output
  parameters during an offline bounce, and `restartComponent` landing
  mid-render truncated Ableton exports whenever the export rate differed
  from the session rate. **Don't route latency through a parameter again.**
  That parameter is gone from the controller's list entirely, and `0x60000000`
  is free — it was never in any chunk we write, so only a host's own project
  cache could still reference it, harmlessly.
  Either way the controller calls
  `restartComponent(kLatencyChanged)`, and the host reads
  `getLatencySamples()` — **that read is the acceptance**: it consumes
  `_pending_latency`, writes `_accepted_latency`, and the next `process`
  issues `Accepted_latency` to the kernel. `setActive(true)` consumes any
  still-pending proposal too, for a host that re-activates without querying.
  The getter has to complete it because FL Studio queries and keeps
  processing, never toggling `setActive`; the proposal would otherwise stay
  pending forever while the host compensated for a latency the kernel had
  not applied. This matches AUv2's `GetLatency`. Don't simplify the rest of
  it — it's how the state machine survives `kDistributable`.
- **State travels as `IMessage`s both ways.** Controller → processor `tiny/state/edit` (a
  `{base, next}` patch, tag = edit sequence), sent from `sync` on the UI thread; processor →
  controller `tiny/state/snapshot` via a 60 Hz `Relay`, only when the processor writes. The
  controller parks snapshots in a `Snapshot_inbox` (`notify` may run on any thread) and
  `sync` applies them. `state::connect_remote` is the wiring.
- **Blocks travel as `IMessage`s** (`tiny/blocks`, tag = address). `process` posts into
  an outbox `blocks::Mailbox`; a 60 Hz `Relay` (scoped to `setActive`, like the latency
  relay) reads it and sends each fresh frame; the controller checks the size and posts into
  its own lock-free mailbox. Not output parameters (too large) and not
  `IDataExchangeHandler` (rejected earlier — see `messaging.hpp`).
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
  `Alg_state` to be trivially relocatable. The init callback reads the rate from the
  `AddSampleRate` context field, so the kernel's allocating `reset(sr)` happens off
  the real-time thread. `ResetFieldData` fills a separate `Reset_state` block with a
  current snapshot of params + render mode, which the init callback adopts *before*
  `reset` — private data is wiped at every reset, so nothing else survives.
- **Everything flowing outwards uses Direct Data**
  ([direct_data.cpp](wrappers/aax/source/direct_data.cpp)): meters, worker
  messages and latency proposals are staged in a `Byte_ring`
  ([byte_ring.hpp](wrappers/aax/source/byte_ring.hpp)) inside private data and
  copied across by `ReadPortDirect` — a memcpy of a byte range, the AAX analogue
  of a VST3 `IMessage`. The wakeup is **~30 ms and not guaranteed regular**, so
  nothing may assume a rate. The producer never overwrites unread data (a full
  push drops), which is what lets the remote consumer read without a seqlock retry.
- **State has its own private-data fields.** `State_inbox` (data model → algorithm): Direct
  Data writes a patch only while `posted == taken`, then bumps `posted`; the algorithm stages
  it at the top of the next block and sets `taken`. The data model's single-slot outbox
  refuses while it is full, so the editor folds later edits into the next patch — nothing is
  dropped. `State_outbox` (algorithm → data model, only when the processor writes) is a
  `Block_store` read the same way as blocks. Private data is rebuilt at a reset, so
  `Reset_state::state` seeds a **freshly constructed** `Alg_state` with the data model's
  last-sent document, its edit sequence and snapshot generation — never a surviving one.
- **Blocks bypass the ring.** Each address has its own private-data field (`blocks[I]` in
  `Alg_context`, a `Block_store`: `seq` + two slots, [block_store.hpp](wrappers/aax/source/block_store.hpp)).
  The algorithm fills the back slot and bumps `seq`; Direct Data reads `seq`, copies the
  front slot, re-reads `seq` and forwards only if it didn't move, via `SetCustomData`
  (`'tBLK'`). `seq == 0` means nothing published since the store was constructed — hold,
  never forward zeros.
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

- **Persistence surfaces**: per-param scalar (host-managed),
  editor `State_map` (string→variant<bool,int32_t,double,string>), the
  `state::Model` record (see "Core abstractions"), and the optional buffer-source persistence from
  [plans/buffer-system.md](plans/buffer-system.md) for large audio buffers
  (not yet implemented).
- `State_adapter` ([state_adapter.hpp](libs/tiny_core/include/tiny_core/state_adapter.hpp))
  is the format-agnostic glue: a JSON document with `version`, `params`,
  `editor` and (with a state model) `state` keys. Editor keys beginning `tinyplug-` are
  the framework's (`drop_reserved_keys`). The same adapter serves both bundle presets and the
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
([tiny_notifications.hpp](libs/tiny_core/include/tiny_core/tiny_notifications.hpp):
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
5. Next `process`, wrapper calls `reset(Reset::Latency{N})` on the kernel and
   the kernel must immediately match (assertion checked).
6. `Host_bypass::set_latency` is updated in lockstep so soft-bypass
   PDC compensation tracks.
7. A `configure` arriving mid-handshake **supersedes** it: the wrapper clears
   `_pending_latency` / `_accepted_latency` so the old proposal cannot be
   applied against the new configuration, and leaves `_reported_latency` alone
   so the post-configure comparison still knows what the host was told.
   VST3 `setupProcessing` · CLAP the fall-through in `activate` · AUv2
   `Cleanup` · AUv3 `deInitialize` · AAX `adopt_reset_state`.

Two numbers, two meanings: `_latency` is what the host holds, `_reported_latency`
is what we last asked for. They diverge only inside the async window, which is why
the dedupe guard cannot fold into `_latency`. Notify whenever the host's number
became wrong — from a proposal *or* a configure — and never when it did not move.

**Runtime proposals are suppressed during an offline bounce** in VST3, CLAP, AUv2 and
AUv3 — the same gate the meters use. A bounce cannot usefully renegotiate delay
compensation, and every format's "tell the host" mechanism interrupts the render
(`restartComponent`, `request_restart`, `PropertyChanged`). The pending value survives, so
the latency getter still completes the handshake at the host's next query. AAX needs no
gate: Pro Tools never changes sample rate under a live instance. The cost of the gate is a
proposal the kernel holds un-acked until that next query — acceptable only because the
quality swap that raises it is pre-emptive.

If you find yourself adding a special-case latency flow, you're probably
fighting this protocol. The full contract, its known limits and the per-format
accept points are in
[lifecycle-migration-handoff.md](plans/lifecycle-migration-handoff.md) §3.

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

## Logging and lifecycle probing

[tiny_log.hpp](libs/tiny_core/include/tiny_core/tiny_log.hpp) /
[tiny_log.cpp](libs/tiny_core/source/tiny_log.cpp) is a realtime-safe logging layer;
[lifecycle_probe.hpp](libs/tiny_core/include/tiny_core/lifecycle_probe.hpp) is a fixed
vocabulary for the processor lifecycle and the latency handshake layered on top.

**The wrappers carry no probes right now.** They were instrumented while the latency
handshake was being repaired and then stripped so the lifecycle diff stays readable; the
layer is kept intact so re-instrumenting is a matter of adding a `log::Probe` member back
and calling the verbs below. Re-instrument all five with the same verbs, so a Pro Tools
trace and a Logic trace of the same plug-in differ only where the formats genuinely differ.

- **Realtime safety.** A log call captures its arguments into a fixed-size POD `Record`
  and pushes it onto a bounded MPMC ring; a drain thread does all formatting and I/O.
  Nothing allocates, locks or formats on the calling thread. Dynamic strings
  (`string_view`/`std::string`) are copied into a 24-byte inline buffer — `const char*`
  is stored by pointer and must be a literal.
  The ring is *not* `Lock_free_queue<..., mpsc>`: that one keeps a registry of writer
  threads that is never reclaimed, and a logger gets called from whatever threads the
  host happens to own.
- **Compiled out by default.** The CMake option `TINY_LOG` is ON for Debug and OFF
  otherwise, and sets `TINY_LOG_ENABLED` **PUBLIC** on `tiny_core`. With it off
  every macro is `((void)0)` (arguments unevaluated) and every `log::Probe` method is an
  empty inline.
- **Multi-process by design.** An AUv3 extension, a distributable VST3's two halves and
  clap-validator's per-test forks are separate processes, so the default sinks are the
  ones that merge them: `os_log` (subsystem `com.tinyplug.log`) on Apple,
  `OutputDebugString` on Windows, plus stderr. Every line carries the pid; instance
  numbers restart per process and only mean something read together with it.

Reading it: `log stream --predicate 'subsystem == "com.tinyplug.log"'` on macOS,
DebugView on Windows, or stderr for CLI hosts (auval, clap-validator, the AAX validator).
The category becomes the os_log category, so
`'subsystem == "com.tinyplug.log" && category == "latency"'` narrows natively. Levels map
so the lifecycle/latency narrative is visible with no flags: `info`/`warn` →
`OS_LOG_TYPE_DEFAULT` (persisted), `debug` → `OS_LOG_TYPE_INFO` (add `--info`), `trace` →
`OS_LOG_TYPE_DEBUG` (add `--debug`).

Runtime configuration, read once at init:

| Variable | Effect |
|---|---|
| `TINYPLUG_LOG` | `trace`/`debug`/`info`/`warn`/`error`/`off`. Default `debug` (Debug builds), `warn` otherwise. |
| `TINYPLUG_LOG_CATS` | Comma-separated category names, or `all`. Default all. |
| `TINYPLUG_LOG_FILE` | Append a copy to this path. Off unless set. |
| `TINYPLUG_LOG_STDERR` | `0`/`1` to force. Default on, except on Apple when the syslog sink is active and the process has no controlling terminal — a GUI host's stderr is routed into the unified log, so both sinks would log every line twice. |
| `TINYPLUG_LOG_SYSLOG` | `0` to silence os_log / OutputDebugString. |

Categories are a bitmask (`lifecycle`, `latency`, `params`, `state`, `worker`, `editor`,
`process`, `host`, `graphics`, `general`) so a probing session can ask for one subsystem.
Per-block logging lives at `trace` and is therefore off unless asked for.

Writing to it:

```cpp
TINY_LOG_INFO(latency, "pending={} accepted={}", pending, accepted); // {} placeholders
TINY_LOG_SCOPE(lifecycle, "setupProcessing");  // logs entry, exit and elapsed time
TINY_LOG_THREAD(audio);                        // tag this thread, once, at the top of process
TINY_PROBE(_probe, host, info, "custom line frames={}", frames); // free-form, but tagged
```

`std::optional` prints as `-` when disengaged, which matters because "absent" is the
interesting half of most latency state.

A wrapper object owns a `log::Probe` with a tag naming its half of the format —
`"VST3/proc"` and `"VST3/ctrl"`, `"AAX/alg"` and `"AAX/model"`, `"AUv3/kernel"`, `"AUv2"`,
`"CLAP"`. The probe constructor is what initializes the layer, so **construct probes off
the audio thread** — that is the only reason the audio thread never runs the one-time
setup. It also claims the `main` thread role if the constructing thread has none, which is
correct in all five formats and saves tagging every main-thread entry point by hand.

The probe verbs mirror the contract in
[lifecycle-migration-handoff.md](plans/lifecycle-migration-handoff.md) §2 and §3:
`configured` / `activated` / `cleared` / `snapped` / `render_mode_changed`, and
`latency_proposed` / `latency_dropped` / `latency_notified` / `latency_queried` /
`latency_accepted` / `latency_mismatch`. **`latency_dropped` is the important one** — a
proposal suppressed by the dedupe guard, by AAX's PDC-disabled guard, or by a configure
that superseded it is otherwise completely invisible, and silent drops are the failure
mode this protocol has.

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
  namespaces (now joined by `process`), `CMakePresets`, the worker `Model` restructure, and the `tools/`
  consolidation. What remains is an optional backlog —
  **[refactor-ideas.md](plans/refactor-ideas.md)** (CI, clang-format, PCH/unity,
  further namespace passes, `detail/` split, downstream migration).

- **[processor-api-migration.md](plans/processor-api-migration.md)** — hand-off guide for
  porting a downstream plug-in repo to the current processor API. Five changes in
  dependency order, with the contracts and the traps.

- **[processor-lifecycle.md](plans/processor-lifecycle.md)** — scheduled next.
  Replaces `reset(double)` with `configure(Config)` so parameter values arrive at
  configuration time, folds `clear` into `reset`, reduces `Render_event` to
  `{Set, Ramp}`, and moves latency onto a `Latency{accepted, proposed}` struct in the
  process context. Closes the AAX-only ordering constraint where `handle`
  precedes `reset`.

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
