# CLAUDE.md

Operational notes for agents working on tinyplug. This file is descriptive,
not normative — when the code disagrees with it, trust the code and update
this file.

## What this project is

tinyplug is a C++20 audio plug-in framework that wraps a single user
processor/editor pair into AAX, AUv2, AUv3, CLAP, VST3 and web binaries. The
user writes format-agnostic code; per-format wrappers under [wrappers/](wrappers/)
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
    (macOS/iOS/Windows/web). Headers `<tiny_platform/...>`, sources in `source/`, config
    templates in `cmake/`. Static lib; links core PUBLIC, Skia PRIVATE.
  - [libs/tiny_dsp/](libs/tiny_dsp/) — header-only DSP helpers (`Host_bypass`,
    `Linear_ramper`, `Delay_line`), `<tiny_dsp/...>`. INTERFACE lib, pure leaf.
  - [libs/tiny_ui/](libs/tiny_ui/) — header-only editor helpers the demos draw with
    (`Controls`, `Theme`, Skia rects and a pixel font), `<tiny_ui/...>`, namespace `tiny::ui`.
    INTERFACE on top of tinyplug; Skia stays the consumer's PRIVATE link.
  - [libs/tiny_text/](libs/tiny_text/) — editor text, `<tiny_text/fonts.hpp>`, namespace `tiny::text`: embedded
    fonts (static or variable, never registered with the OS), HarfBuzz shaping and layout through SkParagraph,
    libgrapheme for unicode (no ICU), the platform's rasteriser. STATIC, model-free, Skia PRIVATE; one `Fonts` per
    editor. Single lines are placed by tiny_text, not SkParagraph: anchors (cap, x, ink, box, baseline), designer
    tracking (none trailing), optical alignment, overflow, from the painted (rounded) baseline. Spans, case
    transforms, outline metrics. [text_demo](examples/text_demo/) is its demo; the other demos keep tiny_ui's
    pixel font.
  - [wrappers/](wrappers/) — one wrapper per format: `aax`, `auv2`, `auv3`, `clap`, `vst3`, `web`.
  - [examples/](examples/) — demo plug-ins consumed by CI.
  - [cmake/](cmake/) — `helpers.cmake`, `plug_info.hpp.in`, etc.
  - [template/](template/) + [tools/new_plugin.py](tools/new_plugin.py) — scaffold
    a new plug-in (a simple gain effect, no worker): `python3 tools/new_plugin.py
    "My Plug" --manu Acme --id plg1`. Generates `examples/<snake_name>/` and appends
    it to [examples/CMakeLists.txt](examples/CMakeLists.txt).
  - [tools/](tools/) — author helper utilities: the scaffold above, preset exporters
    ([tools/presets/](tools/presets/)), AAX page-table generation
    ([tools/pagetables/](tools/pagetables/)), and **contract checks**
    ([tools/contract_checks/](tools/contract_checks/)): `make_contract_checks(<lib>)` builds a
    per-plug-in program checking what the framework requires of a plug-in, without a host. It's the
    home for any new check of that kind; its README lists the planned ones. All opt-in.
  - [plans/](plans/) — design docs for work not yet landed (see "Plans"). A plan is deleted once
    it lands; what it taught moves into this file.
  - [tests/](tests/) — one CTest executable per file; presets `tests`, `tsan`, `asan`
    (`cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan`). New tests use
    audio_bench, fetched at a pinned commit; see [tests/README.md](tests/README.md). Run the
    `tsan` preset after touching any threading primitive. Fake hosts (`tests/hosts/`: CLAP, VST3,
    AUv2, AUv3, and AAX with the wrapper compiled in) drive the real demo bundles under the `tsan-hosts` / `asan-hosts` presets; run them
    after touching a wrapper. Against AutomationDemo's Phase Out probe they also **bounce**: scripted
    host profiles (Logic's pre-roll, a midstream chase, an export that re-runs setup, ...) rendered
    twice after different histories must be bit-identical and equal a fresh instance. Reproducible
    bounces are a hallmark; a change that breaks one fails there. On Windows: `windows-tests`, `windows-asan`, `windows-asan-hosts`
    (MSVC ASan only; CLAP, VST3 and AAX hosts).

## Build

External SDKs live in a sibling [tiny_deps](../tiny_deps) repo. The simplest path
is the CMake presets (they default `TINY_DEPS_PATH` to `../tiny_deps` and
`TINY_INSTALL_PLUGINS=OFF`):

```
cmake --preset debug && cmake --build --preset debug   # Makefiles, all formats except AUv3
cmake --preset macos && cmake --build --preset macos   # Xcode, incl. AUv3
```

Presets: `debug` (Unix Makefiles → `build-debug`), `macos` / `ios` (Xcode →
`build-macos` / `build-ios`, required for AUv3), `windows` (VS, Windows hosts only),
`web` / `web-nonisolated` (Emscripten; see "Web").
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

## Validating AUs with auval

`auval` finds a unit by type/subtype/manufacturer only, and AUv2 and AUv3 share those codes, so
it validates whichever the system resolves — the AUv3 when both are registered. Make the other one
unfindable first, and always confirm from the log, which prints `This AudioUnit is a version 2
implementation.` or `... version 3 ...` (the AUv3 also prints `Loaded AudioUnit out-of-process: true`).

- **AUv2**: macOS discovers an AUv3 `.appex` anywhere on disk — build dirs, copies outside them.
  Find every matching one (`mdfind 'kMDItemFSName == "<Name>_AUv3.appex"'`), `lsregister -u` its
  `.app`/`.appex` and delete it, then `killall -9 AudioComponentRegistrar`. `pluginkit -e ignore`
  is not reliable and persists until `pluginkit -e use -i <bundle id>`.
- **AUv3**: move `~/Library/Audio/Plug-Ins/Components/<Name>.component` aside, restart the
  registrar, build the `<Name>_app` + `<Name>_extension` targets (building registers them).
  Restore the component and delete the AUv3 build afterwards.
- The Xcode presets set no signing team; a local build takes
  `cmake --build --preset macos --target <T> -- CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM=`.
- Run `auval -strict -v aufx <subt> <manu>`. A hang with 0% CPU on an AUv3 is a crashed
  extension: look for `~/Library/Logs/DiagnosticReports/<Name>_AUv3-*.ips`. In zsh, `log` is a
  builtin — read the unified log with `/usr/bin/log show`.

## Core abstractions

The plug-in author implements two classes plus a few static models. Concepts
live alongside each interface: `process::Interface`, and a `concept Model` per model.

- **`Processor`** ([tiny_process.hpp](libs/tinyplug/include/tinyplug/tiny_process.hpp))
  — the whole process side lives in **`tiny::process`**, *including the user's class*, so
  a processor file writes the vocabulary unqualified:
  `configure(const Config&)`, `reset(const Reset::Any&)`, `handle(const Event::Param&)`,
  `process(Context&)`, `latency_samps()`, `tail_samps()`. The concept is
  `process::Interface`, and its requirements follow the plug-in's declared capabilities
  (`#if TINY_HAS_*`): notes in requires `handle(Note::Any)` and `handle(Control::Any)`, a
  work model replying to the processor requires `handle_worker_reply`.

  The user's classes live by **side**: `process::Processor`, `edit::Editor`,
  `work::Worker`. Framework vocabulary is mostly still in `tiny` and resolves unqualified
  from all three. Worker message types are part of the work model, so a processor reaching
  one writes `models::Tick`; see [worker_demo](examples/worker_demo/).
  Two axes, and the split is what to remember: **`configure` allocates** (sample rate
  plus the parameter values to come up holding, plain space) and is the only tier
  permitted to; **`reset` never does**. `Reset::Any` is a closed sum of block-boundary
  commands, facts the processor must adopt to render correctly: `Restart{params}` (the input
  stream is discontinuous — forget transient history and come out exactly as
  `configure(sr, params)` would, except the accepted latency, which it keeps and proposes
  away from) and `Latency{samples}` (the host accepted a proposal — adopt it now,
  `latency_samps()` must match on return). An exhaustive `std::visit` is the idiom, and it
  is what stops a kernel from silently ignoring an accepted latency.
  **The processor is told what, never why.** Everything live — automation, preset and state
  loads, flushes, edits — is events, realized by the processor's own policy; a restart carries
  no reason, so it can't treat history differently by cause. The contract checks tool
  ([tools/contract_checks](tools/contract_checks/)) checks it on a processor alone (any history,
  then `Restart{b}` ≡ `configure(b)`); every demo runs it.
  Events are interleaved with `process` calls by the wrapper so DSP code
  always sees them at the right sample offset (sample-accurate automation
  including ramps).
- **When to restart is one rule, `process::Stream_sync`**
  ([stream_sync.hpp](libs/tinyplug/include/tinyplug/stream_sync.hpp)), used by every wrapper but
  AAX: a restart at a host reset, the render-mode edge, a fresh `configure`, and on resuming
  after the bypass skip (the processor missed that audio: a gap like a seek) — and nowhere else.
  `restart_values` builds the values: the store at the top of the block with that block's
  offset-0 `Set`s folded in, so an in-band first value lands exactly instead of gliding in from a
  stale one. **A stream restart forgets note names** (`Note_io::clear`), so it must come before the
  block names any note: a note-on named first loses its note-off and sounds forever. VST3 collects
  parameters, restarts, then names notes; AUv2 and web set raw MIDI aside and name it after.
  A stream restart also clears and snaps `Host_bypass`, so **ask `can_skip_effect()` again
  after it** before deciding whether to run the processor: asked before, a block un-bypassed at a
  discontinuity skips the processor and the snapped bypass mixes in an unwritten output buffer.
  **AUv2 holds offline renders**: after a discontinuity the processor isn't run (silence
  out) until the first block reaching timeline position 0, because Logic sends no automation for
  its pre-roll while `Globals()` still holds the last locate's values. Real input or a note, a
  missing position, leaving offline and a one-second cap each end a hold. Realtime never holds.
  Enable it in another format only on evidence. AAX rebuilds the processor through `configure`
  at its resets, so its one restart is the skip resume. Design and the Logic measurements:
  [bounce-param-authority.md](plans/bounce-param-authority.md).
- **`edit::Editor`** (concept `edit::Interface`, asserted in every demo) — built from an `edit::Context`
  ([tiny_edit.hpp](libs/tinyplug/include/tinyplug/tiny_edit.hpp)), which hands it **one handle per piece of
  framework-owned state**: `params` (gestures and sets: `start` / `set` / `end`, knob space), `state` (the
  document), `view` (the view model), `size` (the window: `request(w, h)`), joined into undo steps by
  `undo_redo`; plus `notes`, `tasks`, `format`. Receives `Plugin_state` (read-only params, meters, blocks) on
  draw. Only the parameter handle queues (`Action_queue`, as `edit::Action::Any`: `Start` / `Set` / `End`),
  because parameter edits must become ordered host gestures, which wrappers translate into host-native
  begin/edit/end. A size request is held in the `Edit_store` and applied at the frame's end through the
  wrapper's `set_apply` hook, the same path an undo of a resize takes. Editor never shares memory with the
  processor.
- **Edits and undo** ([edit_stream.hpp](libs/tinyplug/include/tinyplug/edit_stream.hpp),
  [undo_history.hpp](libs/tinyplug/include/tinyplug/undo_history.hpp)). `edit::Action::Start`/`End` take an
  **optional** address: with one it's a gesture on that parameter, without one a transaction
  (`undo_redo.begin()`/`end()`/`transaction(fn)`). `Action_queue::normalize` (the `Edit_stream`) gives every
  set a gesture — a bare set its own, a set inside another gesture or a transaction is touched before its
  first set and released when all close — so **wrappers only ever see addressed, balanced starts and ends**,
  one touch per parameter. One undo step is everything inside the outermost start/end, across frames, and
  **a step stays open until the end of the frame it closed in**: an edit appended later that frame (a linked
  control's follower set after a click) joins it. That's what replaced `Action_queue::sort()`; don't
  reorder the stream. A step holds parameter changes and up to one change per state source (`Document`,
  `View`, `Size`); a transaction commits every source as it closes, and one still on the wire (a document
  the processor also writes) holds the step (`can_undo` false) while that source keeps new edits local
  until it confirms. `undo_redo.begin()` is a direct call too: it records earlier uncommitted state as its
  own step before queueing its start. View teardown (`view_impl::close_session`) ends anything left open.
- **Process events and edit events are different types, and the difference is the value
  space.** `process::Event::{Set, Ramp}` carry **plain** values to the kernel;
  `edit::Action::Set` carries **knob** values from the editor / undo history (same layout, value first). They used to be one
  type, which made a wrong-space assignment — the bug shape this codebase is most prone to
  (see "Three spaces") — compile silently. `Value_helper` is the bridge, at the wrapper
  boundary where it always was. `Change_set` is the one genuinely two-sided container and
  is templated on the event type so each instance declares its space.
  *Everything in `process::Event::Any` (params, notes, controls) carries a frame offset*, which
  is what makes the `Event::Ordered` sort meaningful. Anything the host says at a block boundary is a
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
  `Context::blocks`, a `Writer`), `Mailbox` is one lock-free `Data_port` per address,
  `Frames` is the editor's retained copy and `View` is what `Processor_state::blocks` hands
  it (`latest<A>()` / `fresh<A>()`). There is no `Host` mailbox for blocks: the VST3
  controller's `notify` can run on the relay's thread. Optional.
- **`state::Model`** ([tiny_state.hpp](libs/tiny_core/include/tiny_core/tiny_state.hpp)) — one
  trivially copyable struct both sides edit, synchronized and undoable; `writers`
  (`Editor`/`Processor`/`Both`) removes whatever the other side may not do. Every change is a
  `{base, next}` patch applied as a **byte merge** ([state_merge.hpp](libs/tiny_core/include/tiny_core/state_merge.hpp)),
  so a stale edit never erases a concurrent write. The ack is two watermarks (`applied`,
  `rejected`), and that is only sufficient because of a **refusal barrier**: each patch carries
  the newest refusal the editor had heard of (`Edit_header::seen_rejected`), and the store drops
  a patch diffed before its latest refusal. Without it a later patch applied past an unseen refusal
  and retired the refused gesture as landed; don't remove it. Two details keep it from costing more
  than one round trip: a dropped stale patch is **not** recorded as a refusal (its gestures are
  re-armed when the editor hears of the real one; recording it made each in-flight patch stale in
  turn, and a drag through one conflict landed nothing until it stopped), and a patch diffed after
  a newer refusal **replaces** a staged stale one rather than coalescing with it (coalesced, the
  pair was dropped as stale and the fresh gestures were lost). The processor side (`Processor_side`,
  `Store` in [state_store.hpp](libs/tiny_core/include/tiny_core/state_store.hpp)) applies a staged
  patch at the top of the block and triple-buffers what it publishes; `Context::state` is
  a per-block `Access`. The editor side is the wrapper-owned `state::Editor_link`
  ([tiny_state_link.hpp](libs/tinyplug/include/tinyplug/tiny_state_link.hpp)) — it holds the view,
  retains gestures to re-run on refusal, flushes once per frame (`edit::Connection::sync_state`),
  and records document steps into the wrapper's `Undo_history` through `bind_state`. The
  editor sees it as `edit::Context::state`. **Persisted** as a record
  ([state_record.hpp](libs/tiny_core/include/tiny_core/state_record.hpp)): the author's
  optional `save`/`load` pair inside a framework container, appended after the bypass in the
  CLAP stream and the VST3 processor chunk, under `tinyplug-state` in AUv2/AUv3/AAX (base64 in
  AAX), and as `"state"` in preset JSON. A restore decodes it (default on anything refused),
  loads the processor copy (`on_session_load`) and calls `Editor_link::load`, which folds the
  change into the host-load undo step. AAX's algorithm doesn't see the load, so its data model
  loads with `Resend::Yes` and flushes. Optional.
- **`view::Model`** ([view_model.hpp](libs/tiny_core/include/tiny_core/view_model.hpp)) — what
  the editor wants back with the session, `models::View` in `models/view.hpp`: `Undoable` (follows
  undo: a displayed preset name, an A/B selection) and `Restored` (never an undo step: a tab),
  both trivially copyable, each optionally versioned with a record's `save`/`load`; strings are
  `Fixed_string<N>`. Optional pure `on_host_load(View&, params, const Load_info&)`. The wrapper's
  `Edit_store` ([edit_store.hpp](libs/tinyplug/include/tinyplug/edit_store.hpp)) holds it and the
  window size; the editor's handle is `edit::Context::view` (`get`, `edit(fn)`, `restored()`,
  `generation`, `loads`). An undoable edit becomes a step at the frame's end, or joins the gesture
  or transaction that's open. **Features on top (preset names, `*`, A/B, linked controls) are
  the plug-in's**; the framework stores, restores, undoes and reports. StateDemo has one: its preset
  buttons load JSON presets through `edit::Context::apply_preset` in a transaction with the name.
- **Sizing** — the editor declares `static constexpr auto sizing() -> Sizing` (`preferred`, `min`,
  `max`, `undoable`; `preferred_size()` still works through `sizing_of<E>()`). The `Edit_store`
  keeps the size with the session, clamps in each format's size check, and with `undoable`
  records a resize the editor brackets with `undo_redo.begin()`/`end()` as a step (`Source::Size`);
  a host-driven resize is never a step. The editor asks with `edit.size.request(w, h)`: held as the size
  at once, applied at the frame's end through the wrapper's `set_apply`. `run_frame` takes no resize policy.
- **Notes** ([tiny_midi.hpp](libs/tiny_core/include/tiny_core/tiny_midi.hpp)) — not a model:
  `TINY_PLUGIN_WANTS_AUDIO` / `TINY_PLUGIN_WANTS_NOTES` in CMake declare what the plug-in
  carries, `tiny_resolve_capabilities` ([helpers.cmake](cmake/helpers.cmake)) derives the kind
  (effect, instrument, note effect) and each format's identity, and `<tiny_models.hpp>` gets
  `TINY_HAS_NOTES_IN` / `_OUT`, and `TINY_HAS_NOTE_EXPRESSION` for `"in;expression"` (MPE:
  declared per format, read by `midi::Mpe` in `Note_io`, switched by an optional processor
  `mpe_enabled()`). `midi::Note` and `midi::Control` (a closed set of
  player controls, never arbitrary CC; re-exported into `tiny::process`) arrive through `handle`
  between slices; output leaves through `Context::notes`, which also takes `midi::Raw`:
  any channel voice message, output only, since sending can't write the plug-in's own
  parameters. Every wrapper owns a `process::Note_io`
  ([tiny_note_io.hpp](libs/tinyplug/include/tinyplug/tiny_note_io.hpp)): `Note_ids` mints one
  id per note whatever the format supplied, the editor's pipe (`edit::Context::notes`) lands
  at the top of the next block, and a stream break (a restart, bypass edge, deactivate)
  sends all-notes-off. MIDI 1.0 goes through one codec
  ([midi_codec.hpp](libs/tiny_core/include/tiny_core/midi_codec.hpp)). Per format: CLAP note
  ports, VST3 event buses plus `IMidiMapping` onto 144 hidden parameters for controls, AUv2
  `MIDIEvent` and the MIDI output callback (factory by kind), AUv3 render events and
  `MIDIOutputEventBlock`, AAX MIDI nodes with the algorithm slicing at packet timestamps and
  editor notes on the inbound ring.
- **`work::Model`** ([tiny_work.hpp](libs/tiny_core/include/tiny_core/tiny_work.hpp)) — the
  worker's four channel variants plus tuning, declared as `models::Work` in
  `models/work.hpp`. Optional, and paired with `worker.hpp` (the `work::Worker` class).

## Model layer

A plug-in declares models in `source/models/{params,meters,blocks,state,work,view}.hpp` and classes in
`source/{processor,editor,worker}.hpp`. Discovery is **file presence, resolved by CMake**:
`configure_models()` / `configure_plugin()` ([helpers.cmake](cmake/helpers.cmake)) generate
`<tiny_models.hpp>` (`models::Resolved`, `User_params`/`User_meters`/`User_work`,
`TINY_HAS_*` + `has_*`) and `<tiny_plugin.hpp>` (`plugin::Resolved`,
`User_processor`/`User_editor`/`User_worker`) from
[tiny_models.hpp.in](cmake/tiny_models.hpp.in) / [tiny_plugin.hpp.in](cmake/tiny_plugin.hpp.in).
An absent model resolves to a zero-entry `<ns>::None`, never a monostate. Adding or removing a model file
re-globs on the next build (`CONFIGURE_DEPENDS`), except for single-target Xcode builds, which
skip `ZERO_CHECK`. Build `ZERO_CHECK` first there. Client porting guide:
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
  `Context::meters` / `edit::Connection::read_meters` fields. `Processor_state::meters` stays,
  empty — it is core. A new meter path must sit behind `TINY_HAS_METERS` too.

## Parameter permanence

A parameter model has **three** independent permanence surfaces, not one.

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

`Infos::au_order()` returns that order as addresses, and
[wrappers/auv2/source/effect.cpp](wrappers/auv2/source/effect.cpp) `GetParameterList`
is its only consumer. A model that doesn't declare `au_order()` falls back to
**address order**, which is append-only by construction since the
`Address` enum is — it costs you control of the AU display order but is never
unsafe. It must never fall back to tree order: that would make the tree itself
append-only, which is exactly the constraint this design removes.
Everything else is free to change: `name`, `short_name`, `Group::name`, and the
whole display hierarchy provided ancestry is preserved. Group *names* (not
identifiers) are what CLAP modules, VST3 units and AUv2 clumps are built from, so
those are cosmetic.

## Three spaces, one parameter

Conversions are everywhere; the central type is `params::Value_helper` in
[value_helper.hpp](libs/tiny_core/include/tiny_core/value_helper.hpp) /
[value_helper.cpp](libs/tiny_core/source/value_helper.cpp) (the declarative model lives
in [tiny_params.hpp](libs/tiny_core/include/tiny_core/tiny_params.hpp)):

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

`Semantics::Real` carries an `Adapter::Any` (`Lin`, `Log`, `Pow`, `Taper`, `Piece`)
controlling the knob↔plain mapping. Conversions through these adapters have asserts on their domains —
log requires `min_val > 0`, taper requires `0 < taper < 1`, etc.

Text is `Bool`/`Int`/`Fixed`/`Real`'s `display` (`Display::Boolean` / `Display::Numeric`:
`Default`, `Percent`, `Decibels`, `Hertz`, `Gain`, ..., and `Custom` with author functions in
plain space, called from any thread). It is formatting only — no host sees a unit — and
`Host_formatter` is its one reader, so the host and the editor show the same string. `List`'s
items are its display. `Spec::description` reaches no host; it is for tooltips, the web
manifest and manuals.

`Infos` keeps **one** copy of the specs, in address order (`param_specs()`, `param_spec(addr)`);
`presentation_order()` and `au_order()` are address lists, so only address order can be indexed
by address. `param_tree()` is the tree as authored.

`params::Policy` (automation/control/hidden/interface) fully enumerates how a
parameter should be exposed and persisted — never flags-and-bits.
`State_rules::is_persistent` is the canonical predicate.

## Thread model and message queues

Communication uses `Lock_free_queue<T, capacity, Queue_concurrency::*>`
([lock_free_queue.hpp](libs/tiny_core/include/tiny_core/lock_free_queue.hpp),
a detemplated farbot port). Common topology:

- **Host → processor**: format-native event stream, normalized into
  `std::vector<Event::Ordered>` per render (with offsets and ramps).
- **Editor → host → processor**: `edit::Action::Any` from the parameter handle goes via
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
is the canonical UI loop: read meters → call the editor's `on_draw` → `resolve_frame` (normalize →
record undo → run a deferred undo/redo → dispatch → keep the frame as `last_frame()` → sync state →
`Undo_history::end_frame`). All wrappers route through it (web's tick calls `resolve_frame` itself);
if you change frame semantics, change them here. Every wrapper calls `view_impl::close_session` right
after the editor's `on_hide`, and binds its history to its queue (`bind_actions`).

## Worker channel

`User_worker` runs on its own thread (`work::Runner` in
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
(`work::Replies_to_processor` / `work::Replies_to_editor`)
inside `try_drain_worker_to_*` *templates* — they have to be templates so
`if constexpr` can discard the branch in a dependent context.

## Per-format quirks

### VST3 (wrappers/vst3/) — TWO-COMPONENT, DISTRIBUTABLE

VST3's defining quirk for this codebase: the processor (`Audio_effect`,
[audio_effect.hpp](wrappers/vst3/source/audio_effect.hpp)) and the controller
(`Controller`, [controller.hpp](wrappers/vst3/source/controller.hpp))
are **distinct COM components** registered separately in
[entry.cpp](wrappers/vst3/source/entry.cpp) with class flag
`Steinberg::Vst::kDistributable`. They share no memory, may live in
different processes, and communicate only via the host through
`IMessage` / `IComponentHandler::restartComponent`. The controller hosts
the editor; the processor hosts the DSP. Two UIDs (`controller_uid`,
`processor_uid`) are generated from the manufacturer/plug-in codes plus
"ctrl"/"proc" suffixes in [helpers.cmake](cmake/helpers.cmake).

This has knock-on effects throughout the wrapper:

- **State is split.** `Audio_effect::getState`/`setState` persists
  param values, bypass and document; `Controller::getState`/`setState` persists the
  editor's half (the legacy map's framework keys, then the edit record);
  `Controller::setComponentState` re-receives the processor's values so the controller's
  mirror stays in sync, and opens the load's undo step that `setState` completes. A 4-word
  header (framework_code, manufacturer_code, plugin_code, count) prefixes each chunk; both
  layouts are written by [chunks.hpp](wrappers/vst3/source/chunks.hpp), which the
  `.vstpreset` exporter shares.
- **Latency changes hop through the host, and the two paths are different.**
  A *configure-time* change (`setupProcessing`) is sent **directly** over
  `IMessage` (`tiny/latency/changed`) — no render is in flight there, and
  `_reported_latency.exchange` syncs the shadow whether or not the send
  lands, so a reconfigure can never be mistaken for a mid-render change.
  A *runtime* proposal (`Context::propose_latency`, raised on the audio
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
  `IMessage`: audio-thread pushes lock-free into `_worker_outbound` and a
  `Relay` drains it and calls `sendMessage` to the controller (on the UI thread, so
  worker traffic stalls while the host's main thread is blocked and the
  outbound queue can fill); replies arrive in `Controller::notify`, get pushed into
  `_worker_to_proc`, and the controller's worker runner `set_post_cycle` posts a
  second relay that ships them back to the processor via another `IMessage`. See [messaging.hpp](wrappers/vst3/source/messaging.hpp)
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
undo, and state chunks). The GUI hangs off the data model.

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
  and each difference becomes a `process::Event::Set`. Don't replace this with a dirty
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
  `Alg_context`, a `Block_store`: `seq` + two slots, [wire.hpp](libs/tiny_core/include/tiny_core/wire.hpp)).
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
  keys (`tinyplug-num-params`, `tinyplug-edit-keys`, `tinyplug-host-bypass`,
  `tinyplug-state`, `tinyplug-edit`), written by
  [chunk_writer.hpp](wrappers/aax/source/chunk_writer.hpp), which the `.tfx` exporter shares.
  Pure data model — untouched by the two-component split. `CompareActiveChunk`
  is required for Pro Tools' compare light; it compares params and the state record.
- **Chunk calls arrive on any thread, concurrently** (the SDK default; don't set
  `AAX_eProperty_RequiresChunkCallsOnMainThread`). Each builds its chunk on the calling
  thread from what is safe anywhere: `_knob_mirror` (every parameter in knob space, written
  on main wherever one changes, since `AAX_CParameter` values aren't atomic), the editor link's
  published copy of the document (`Editor_link::published`, refreshed at each sync and load), the
  `Edit_store`. The built chunk is cached under a key of cheap revisions (parameters, store, document),
  so `CompareActiveChunk` and `GetChunkSize` rebuild only when something moved. A load from off main waits in a `Pending_load`, and is
  the state, until `TimerWakeup` applies it. A `GetChunkSize`/`GetChunk` pair keeps the
  chunk it sized, per thread. Every call uses a local
  `AAX_CChunkDataParser`; never the SDK's shared `mChunkParser`. The SDK's `LoadChunk`
  over-reads truncated chunks — an SDK bug (to report to Avid), exempted from ASan in
  the root `CMakeLists.txt` and `tests/sanitizers/asan.supp`.
- AAX parameters are addressed by **string IDs**, not integers; `tree_to_aax_ids`
  in [adapters.hpp](wrappers/aax/source/adapters.hpp) builds canonical IDs from
  the param tree, and `aax_id_to_tiny` reverses the map.
- Custom taper delegates (`Real_semanticsTaperDelegate`, `Fixed_semanticsTaperDelegate`,
  [taper_delegate.hpp](wrappers/aax/source/taper_delegate.hpp)) exist so AAX's
  normalized-to-plain transform respects our `Knob_adapter`. Likewise
  `Semantics_display_delegate` ([display_delegate.hpp](wrappers/aax/source/display_delegate.hpp))
  routes every user parameter's text through `Host_formatter`, so AAX shows the same strings as
  the other formats; only the master bypass keeps an SDK delegate.
- Latency change protocol: kernel proposes → algorithm pushes onto the return
  ring → Direct Data calls `SetSignalLatency` → AAX delivers
  `AAX_eNotificationEvent_SignalLatencyChanged` → data model reads back what the
  host accepted (the host owns latency in AAX) → next `Runtime_packet` carries it
  with a bumped `latency_seq`, and the algorithm issues `Accepted_latency`.
  The **sequence, not the value**, gates application — a zero-initialised packet
  must not read as "the host accepted zero".

### AUv2 (wrappers/auv2/) — macOS desktop AU

- Built only on Apple non-iOS. Extends `ausdk::AUBase`. Despite being
  "desktop only", it shares no code with AUv3 — the AUv3 wrapper has its
  own `DSPKernel` ([DSPKernel.hpp](wrappers/auv3/source/extension/DSPKernel.hpp)).
- View factory class name must be **unique per plug-in** because AUv2
  hosts load multiple plug-ins into the same process; collisions are
  silent runtime breakage. The user sets `TINY_AUV2_VIEW_CLASS` to a
  unique name; the property is propagated into `Plug_info::Auv2::view_class`.
  [view_factory.mm](wrappers/auv2/source/view_factory.mm) declares
  the class via Objective-C macros.
- `Plug_info::version` is also encoded into the Objective-C class names
  for `mac_view.mm` (see `configure_mac_view` in [helpers.cmake](cmake/helpers.cmake)):
  multiple versions of the same plug-in loaded concurrently would
  otherwise collide on `MacView`.
- Parameter clumps come from the `params::Group` tree via `Clump_map`.
- `kAudioUnitProperty_UserPlugin = 64000` is the convention for plug-in-
  specific properties — see [plug_info.hpp.in](cmake/plug_info.hpp.in).
- Latency change: `_pending_latency` flag, then `PropertyChanged
  (kAudioUnitProperty_Latency)`, then on `GetLatency` we read the pending
  value and store it as accepted.

### AUv3 (wrappers/auv3/) — extension + container app (+ shared framework on iOS)

- AUv3 produces **two targets on macOS** (container app + AU extension)
  and **three on iOS** (container app + AU extension + a shared `*_core`
  framework that holds the actual AU/ViewController/view code). On iOS the
  extension is a literal one-symbol stub (`stub.m`); both the app and
  the extension link the shared framework and load
  `Auv3_AUAudioUnit` / `Auv3_AUViewController` via `NSClassFromString`, so
  the AU code exists once on disk and is shared between the host process
  and the in-process app preview. The framework target is gated by
  `if(CMAKE_SYSTEM_NAME STREQUAL "iOS")` in
  [make_auv3_plugin.cmake](wrappers/auv3/make_auv3_plugin.cmake) — on macOS
  the extension compiles the AU sources directly because each plug-in's
  ObjC class names are already disambiguated via `configure_mac_view`.
- Entitlements, Info.plist, and asset catalogues are templated under
  [wrappers/auv3/cmake/](wrappers/auv3/cmake/). The iOS framework has its
  own `Info-Core.plist` and bundle ID (`<base>.core`).
- iOS and macOS share the wrapper sources. `TINY_IOS_DEVICE_FAMILY`
  selects ipad/iphone/universal (`UIDeviceFamily` in the plist).
- Optional IAP / App-Group entitlements are templated for the container
  app via `TINY_APP_GROUP_ID`, `TINY_APP_IAP_PRODUCT_ID`, `TINY_APP_IAP_TRIAL_ID`.
- Ownership mirrors the other wrappers: the audio unit (`Auv3_AUAudioUnit`) holds the
  editor, `Task_manager`, undo history and worker (`std::optional`s emplaced in init, since
  an Objective-C++ ivar can't take constructor arguments). `DSPKernel` holds the processor and only
  pointers to the worker's two processor-side queues. The view controller borrows the
  audio unit's task manager.
- Param values cached locally in `_hostvalues` atomics (host space)
  because AUv3 sends per-param values and the controller view needs them
  fast.
- **Per render, `DSPKernel::begin_block` runs before any of the render's events**: the accepted
  latency, then values set off the render thread (`_param_changes`), then any restart (folding in
  the render's own events at its start). Off-thread values are older than the render's events, so
  delivered after them they overwrote newer ones; the AUv3 bounce scenario caught exactly that.
- Presets: a generated `preset_list.hpp` enumerates factory presets;
  AUv3 hosts can also save **user presets**. When loaded, the editor
  state map gets an extra key `preset-name` (string) by convention — see
  the README "Presets" section.

### CLAP (wrappers/clap/) — single-component, modern

- The simplest wrapper. Single class `Plugin`
  ([plugin.hpp](wrappers/clap/source/plugin.hpp)) extends
  `clap::helpers::Plugin`. Studio One is noted as misbehaving — `MisbehaviourHandler::Terminate`
  is enabled only in debug builds.
- Param "modules" (slash-separated path strings from `tree_to_clap_modules`)
  give CLAP its hierarchy.
- State and editor map are stored together in one stream with a 5-word
  header (`State_rules::Clap::Header`).
- Has a preset discovery factory ([preset_discovery.hpp](wrappers/clap/source/preset_discovery.hpp))
  so CLAP hosts can enumerate the plug-in's preset folder.
- `_hostvalues` atomics in host space mirror what the audio thread sees
  (similar to AUv3). `_from_flush` queue carries state-load events that
  arrive outside the process callback (CLAP's `paramsFlush`).
- Latency change goes through `_pending_latency` → on next `activate`,
  call `clap_host_latency::changed(host)` → host calls `latencyGet`,
  reads the new value, and we write `_accepted_latency` for the next
  process.

### Web (wrappers/web/) — TWO MODULES, AAX-SHAPED

The browser splits the plug-in across the AudioWorklet and the page's main thread, and the wrapper keeps the split:
two wasm modules joined by rings in shared memory, like AAX's algorithm and data model. The design plan is in git
history; follow-ups are item 4 of [roadmap-misc.md](plans/roadmap-misc.md).

- **Processor module** (`<Base>_processor.wasm`, [module.cpp](wrappers/web/source/processor/module.cpp)): the
  processor alone, standalone wasm with no Emscripten JS, on an imported, fixed-size shared memory. Our own
  [worklet.js](wrappers/web/js/worklet.js) instantiates it. No threads, no Skia. It links with `-sSHARED_MEMORY`
  and no `-pthread` (standalone wasm can't carry pthreads), so libc++abi's mutex is stubbed in module.cpp.
- **Controller module** (`<Base>_controller.mjs/.wasm`, [controller.cpp](wrappers/web/source/controller/controller.cpp)):
  everything else, on the main thread. Param authority (the page is the DAW), editor and view, undo, the remote
  state link, the worker, `Task_manager` on a prewarmed pthread pool. `tick()` stands in for the view loop while
  no editor is open; once one is, its `run_frame` takes the editor's half.
- **The wire** is [`<tiny_core/wire.hpp>`](libs/tiny_core/include/tiny_core/wire.hpp) (AAX's `Byte_ring` and
  `Block_store`, promoted), laid out per channel in [wire_layout.hpp](wrappers/web/source/wire_layout.hpp) and
  mirrored in [wire.js](wrappers/web/js/wire.js). **One writer per ring.** The
  [pump](wrappers/web/js/pump.js) is a dedicated worker holding both memories: it moves frames between them,
  routing processor messages by kind so each controller ring has one reader, and keeps moving in a hidden tab.
  Worker traffic moves in `work::Runner`'s post-cycle, on the worker's thread.
- **A host load is a session load, not an edit**: its own store, pumped before edits, landing in the processor's
  `on_session_load`, as CLAP's.
- **Seqlocks need atomic data in wasm.** Binaryen strips `atomic.fence` at -O2, so `Block_store` copies slots
  as relaxed atomic words; `publish_with`'s in-place fill is native-only. Don't "simplify" the word copies to
  `memcpy` — the Release wasm build accepts torn frames if you do.
- **Never block main on a task that proxies to main.** `Task_manager::shutdown` waits by spinning in Emscripten,
  and a task calling back into main deadlocks the tab. Natively the same rule, but there it only stalls.
- **Latency** needs no negotiation: a proposal is accepted at once and lands as `Reset::Latency` next block.
- **Two variants, one tree each** (`TINY_WEB_VARIANT`, presets `web` / `web-nonisolated`). `isolated` needs a page sent
  with COOP / COEP; `nonisolated` runs on any page: no shared memory, so no threads (`TINY_PLATFORM_THREADS` 0:
  `Task_manager` runs every lane on main in `run_main`), the pump becomes [link.js](wrappers/web/js/link.js)'s two
  halves over the worklet's port, and a plug-in with a worker is a configure error. Its Skia is
  `build-skia-web.sh nonisolated`. manifest.json's `"variant"` tells `host.js` which wire to use.
- **Platform**: [web_view.cpp](libs/tiny_platform/source/web_view.cpp) draws from requestAnimationFrame, input
  arrives through exports the page's listeners call ([editor.js](wrappers/web/js/editor.js)); CPU raster with
  `putImageData` ([web_context.cpp](libs/tiny_platform/source/web_context.cpp)); `platform::Paths` under an
  IndexedDB mount; browser dialogs. Skia for wasm comes from tiny_deps' `build-skia-web.sh` with its pinned emsdk,
  which the `web` presets also use — one toolchain for every object in a module.
- **Build and test**: `cmake --preset web && cmake --build --preset web && ctest --preset web`. Labels:
  `web_host` (processor module under Node), `web_split` (all three threads under Node), `browser` (headless
  Chrome, muted and offline where it can be). `ctest --preset web-nonisolated` runs the same tests linked
  (`web_split --link`), pages served `--no-isolation`. `node tools/web/serve.mjs` serves `tools/web/editor.html?plugin=<Base>`;
  each `<Base>.web/` is also a self-contained, deployable bundle with its own index.html.

## State / preset model

- **Persistence surfaces**: per-param scalar (host-managed), the `state::Model` record (the
  document), the **edit record** ([edit_record.hpp](libs/tiny_core/include/tiny_core/edit_record.hpp):
  the view model's two parts and the window size, from the wrapper's `Edit_store`), and the
  optional buffer-source persistence from [plans/buffer-system.md](plans/buffer-system.md)
  (not yet implemented). The edit record sits beside the document's: appended to the CLAP
  stream and the VST3 controller chunk (`read_trailing_records` reads either order), under
  `tinyplug-edit` in the AU and AAX containers, `"edit"` in a web session.
- **The legacy editor map** (string→variant, the author's `save_state` before the view model)
  is read forever, since shipped sessions hold it. Its framework keys (`editor-width`/`-height`;
  `tinyplug-preset-name`/`-id`, which the native exporters write and which make a load a
  preset's) come out (`framework_keys::take`); the rest reaches the view model as
  `Load_info::legacy`. Saves write only the framework's size keys there, for older builds.
  Old sessions are pinned by [tests/fixtures](tests/fixtures/README.md).
- **Saves from any thread** (AAX chunks, AUv2 ClassInfo, AUv3 `fullState` and user presets)
  build on the calling thread from what is safe anywhere: parameters (AU `Globals()` and the
  parameter tree are atomic; AAX reads `_knob_mirror`), the document (the processor's copy in
  AUv2/AUv3, through `Processor_side::snapshot`, whose non-audio readers share a mutex; the editor
  link's published copy in AAX, whose processor lives in the algorithm), the `Edit_store`'s
  published copy. AUv2 guards the base class's current preset (`_base_mutex`, recursive: the base
  notifies PresentPreset listeners with it held). A load from off main waits in a
  `Pending_load` ([pending_load.hpp](libs/tiny_core/include/tiny_core/pending_load.hpp)) and is
  the state meanwhile, so a save straight after it returns what was loaded; main applies it
  when a one-shot `Relay` delivers (AUv2, AUv3) or at `TimerWakeup` (AAX). No polling.
  CLAP and VST3 need none of this — their specs put state calls on the main/UI thread.
- `State_adapter` ([state_adapter.hpp](libs/tiny_core/include/tiny_core/state_adapter.hpp))
  is the format-agnostic glue: a preset is `version`, `params`, `state` (the document, base64),
  `name` and `id` (`preset_state(Preset_info)`); a web session adds `edit` and the framework's
  `editor` keys (`session_state()`). `editor` from older files is still read.
- Per-format chunk layouts in [state_rules.hpp](libs/tiny_core/include/tiny_core/state_rules.hpp).
  Each format embeds a (framework/manufacturer/plugin) sentinel; mismatch
  asserts.
- Presets are JSON files with a user-chosen extension (default `.json`);
  CMake (`copy_presets` in [helpers.cmake](cmake/helpers.cmake)) places
  them into bundle Resources for AAX/AUv2/AUv3/CLAP/VST3 macOS bundles, and
  the format-native preset directory at install time on macOS/Windows.
  AAX uses `.tfx` placed in the bundle; VST3 uses `.vstpreset` placed by
  the installer. Both are written by the exporters in [tools/presets](tools/presets/) and checked in;
  [examples/state_demo](examples/state_demo/) is the worked example (`presets/` and `presets/out/`).

### Host-initiated loads → one undo step, reported to the view model

When the host loads a preset / full state, each format's restore entry point
(VST3 `setComponentState`+`setState`, CLAP `_update_state`, AUv2 `RestoreState`,
AUv3 `setFullState`, AAX `SetChunk`, web `load_state`) snapshots all param values in **knob space**
before and after applying the load, then calls
`Undo_history::push_host_load(before, after, out_changes)` to record **one
coalesced undo step**. The document joins it (`Editor_link::load`, `amend_host_state`), then
the editor's half: `load_editor_state(store, legacy_map, edit_record, params, info)`
([edit_store.hpp](libs/tinyplug/include/tinyplug/edit_store.hpp)) restores the view and the
size, and calls the view model's optional pure `on_host_load(View&, params, Load_info)`, all
inside the same step. `Load_info` says what happened, never what it means: session or preset,
the preset's name and id when the path knows them (JSON `name`, the file name, the factory
list, the exporter's key), the legacy map. A session without an edit record loads the view's
defaults; a preset keeps the view. The `Undo_history` and `Edit_store` live on the long-lived
wrapper class, so all of this happens **with the window closed too**. VST3 splits the load:
the step opens in `setComponentState` and the editor's half lands at the end of `setState`.
There is no `notify`, `save_state` or `load_state` on the editor: dark mode is
`View_context::dark` each frame, a load's edge is `edit.view.loads()`.

## Latency change protocol (consistent across formats)

Every wrapper implements the same pattern:

1. Kernel sets `Context::propose_latency = N` during `process`.
2. Wrapper stores `_pending_latency = N` (atomic, lock-free
   `optional<uint32_t>`).
3. Wrapper signals the host "latency may have changed" using the
   format-native mechanism — see each format above.
4. Host calls back; wrapper reads `_pending_latency`, stores
   `_accepted_latency = N`, reports `N` to the host.
5. Next `process`, wrapper calls `reset(Reset::Latency{N})` on the kernel and
   the kernel must immediately match (assertion checked).
6. `Host_bypass::set_latency` is updated in lockstep so soft-bypass
   PDC compensation tracks. It never allocates: the bypass delays are sized at
   `configure` by `set_max_latency(process::max_latency_of(processor))`, from the
   processor's optional `max_latency_samps()` (absent means a fixed latency). A
   proposal beyond that is refused by `Host_bypass::admit` (assert in debug), never
   grown into on the audio thread.
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
fighting this protocol.

## Platform layer

[libs/tiny_platform/](libs/tiny_platform/) handles per-OS view, dialogs, paths,
and the Skia surface. Selection is `#if TINY_PLATFORM_MACOS / TINY_PLATFORM_IOS /
TINY_PLATFORM_WINDOWS / TINY_PLATFORM_WASM`, at compile time only, and so is each
OS's CPU/GPU backend. Planned backend work: [roadmap-misc.md](plans/roadmap-misc.md).

- macOS: Cocoa view + Metal-backed `SkSurface`; per-plug-in ObjC class
  names generated by `configure_mac_view` in [helpers.cmake](cmake/helpers.cmake)
  to avoid runtime ObjC class collisions when many plug-ins are loaded
  in one process.
- iOS: UIKit + Metal, AUv3 only. `IOS_GRAPHICS_GPU=0` selects a CPU raster backend
  composited by CoreAnimation, the escape hatch from `CAMetalDisplayLink` GPU timeouts.
- Windows: Win32 window + Skia CPU backend. The D3D12 path exists but is disabled
  (`WIN_GRAPHICS_GPU` defaults to `0` in
  [libs/tiny_platform/CMakeLists.txt](libs/tiny_platform/CMakeLists.txt)) pending
  non-blocking frame sync.
- Web: a `<canvas>`, Skia CPU raster blitted with `putImageData`; see "Web" above.
- **`platform::Text`** ([platform_text.hpp](libs/tiny_platform/include/tiny_platform/platform_text.hpp)) opens a native
  single-line field over the editor (`NSTextField` in `mac_view.mm`, `UITextField` in `ios_view.mm`, a subclassed
  `EDIT` in `win_view.cpp`, an `<input>` in `web_view.cpp`) in the editor's embedded font; its callback fires once,
  through the `Task_manager`, like the dialogs. Editors include it from their `.cpp`: the web processor module
  includes `editor.hpp` without linking `tiny_platform`. A press anywhere else in the editor ends a field (none of
  the views takes focus). Windows: the editor window has `WS_CLIPCHILDREN`, and a variable font is matched by GDI's
  per-instance names from DirectWrite, not the name table's family. Design and the host matrix:
  [keyboard-input.md](plans/keyboard-input.md).
- `Relay` delivers on the main thread everywhere. On Windows that is a per-DLL hidden
  message window bound to the thread that constructs the first relay, so **construct
  relays on the UI thread**. A drain on main must be bounded, never `while (pop())`
  against the audio thread, or it freezes the host. CLAP constructs no relays yet.
- `Task_manager` ([task_manager.hpp](libs/tiny_core/include/tiny_core/task_manager.hpp))
  exposes background / main-thread / serial-queue dispatch. The view is
  the only thing that calls `bind_main` + `run_main`; user code uses the
  `Actor`, which holds the queues weakly and is refused once the manager
  shuts down. **Every owner calls `_tasks.shutdown()` first in its
  destructor** — that is what lets tasks, dialog callbacks and network
  completions capture a raw `this`. AUv3's lives on the audio unit.

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

The probe verbs mirror the processor lifecycle and the latency protocol above:
`configured` / `activated` / `cleared` / `snapped` / `render_mode_changed`, and
`latency_proposed` / `latency_dropped` / `latency_notified` / `latency_queried` /
`latency_accepted` / `latency_mismatch`. **`latency_dropped` is the important one** — a
proposal suppressed by the dedupe guard, by AAX's PDC-disabled guard, or by a configure
that superseded it is otherwise completely invisible, and silent drops are the failure
mode this protocol has.

## CMake mechanics

- Per-plug-in build: an `add_library(<PLUGIN>_lib STATIC)` carrying the
  user code and a bag of `TINY_*` properties (see
  [examples/gain_demo/CMakeLists.txt](examples/gain_demo/CMakeLists.txt) as
  reference). `configure_plug_info` reads those properties and generates
  `plug_info.hpp`. Then `make_<format>_plugin(<TARGET>)` for each desired
  format.
- Plug-in codes: `TINY_MANUFACTURER_CODE` and `TINY_PLUGIN_CODE` are
  four-character codes embedded in AAX/AU/VST3 IDs. Manufacturer code
  needs at least one capital letter (AU convention).
- **Embedded files**: `tiny_embed(<target> NAME <name> FILES ...)` ([embed.cmake](cmake/embed.cmake)) compiles
  each file into the target at build time and generates `<embedded/<name>.hpp>`, one
  `embedded::<name>::<file>() -> std::span<const std::byte>` per file (`InterVariable.ttf` → `intervariable_ttf`).
- `mac_view.mm` is compiled once **per wrapper target** (not per plug-in)
  with a unique ObjC class name so multiple plug-ins coexist in a host
  process — see `configure_mac_view` in [helpers.cmake](cmake/helpers.cmake).

## Style

From the README, enforced informally:

- Herb Sutter "AAA"/left-to-right: most lines start with `const auto`;
  functions use trailing return types (`-> ReturnType`).
- Stroustrup naming with snake_case for multi-word names; types and
  scoped-enum members capitalized (`Value_helper`, `Reset::Restart`).
- Opening brace on a new line only for function definitions.
- Private members prefixed with `_`.

## Plans

Read the relevant plan in [plans/](plans/) before non-trivial changes there. Landed work has no plan;
its rules are in this file and its history in git.

- **[client-port.md](plans/client-port.md)** — next (0.5). Bring all_plugins up on main: the `Restart` port and
  contract checks per product, and the editor features (preset names, `*`, A/B, linked controls, layout) rebuilt
  on the view model and transactions. Holds the 0.4 client defects the port should fix.
- **[fonts-and-text.md](plans/fonts-and-text.md)** — landed for the framework, on every platform; the clients remain
  (client-port.md), plus paragraph tracking and SVG later.
- **[keyboard-input.md](plans/keyboard-input.md)** — part 1 landed: `platform::Text`, inline native text fields on
  every platform; the host matrix remains. Part 2 is design: key events for shortcuts under per-frame claims; no
  hooks or event monitors, unclaimed keys go back to the host.
- **[buffer-system.md](plans/buffer-system.md)** — design. Managed large audio buffers (looper, granular,
  sampler, drum machine): a processor-owned canonical source, off-thread prepare, pointer-swap install,
  persisted in the session.
- **[headless-plugin.md](plans/headless-plugin.md)** — design. A plug-in with no editor and no
  `tiny_platform` / Skia link. The generated `TINY_HAS_EDITOR` / `edit::None` exist; `configure_plugin`
  still refuses a missing `editor.hpp`.
- **[param-lockfile.md](plans/param-lockfile.md)** — deferred. A checked-in `params.lock` per plug-in so the
  permanence rules above become build-time enforcement.
- **[bounce-param-authority.md](plans/bounce-param-authority.md)** — in the tree, unmeasured in a real DAW.
  `Restart{params}`, `Stream_sync` and the AUv2 hold are in; real Logic measurements remain (the all_plugins port
  is in client-port.md). Holds the Logic diagnosis and evidence.
- **[linux-feasibility.md](plans/linux-feasibility.md)** — feasibility. CLAP and VST3 on Linux.
- **[roadmap-misc.md](plans/roadmap-misc.md)** — design. Windows multitouch, a macOS CPU backend, Windows
  GPU sync, the web format's follow-ups (by-hand checks, test gaps, Graphite on WebGPU, CI), and the host bypass
  from the editor.

## Things to be careful about

- **Don't break the VST3 split-state contract.** `setComponentState` on
  the controller is *not* the same as `setState`. Param chunk lives on the
  processor; the editor's half (edit record) lives on the controller; the controller mirror
  of param values is established only via `setComponentState`.
- **Don't regenerate a fixture to make a test pass** ([tests/fixtures](tests/fixtures/README.md)):
  an old session that no longer loads is the bug.
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
  user's `process::Processor` declares `handle_worker_reply(const To_processor&)`,
  it gets called automatically; if not, the drain is a no-op. The check
  has to live inside a template so `if constexpr` properly discards the
  un-detected branch — don't move it out.
