# Plan: Unified edit model — the framework owns state, the client owns features

> Status: **framework steps 0–6 in the tree on `unified-edit-model`**, uncommitted for review; step 7 (clients)
> remains. Deviations from the text below: `edit.presets.apply` is `Edit_context::apply_preset`; `frame.edits()` is
> `edit.actions.last_frame()`; off-main
> saves in AUv2/AUv3 read the processor's document copy under a mutex (it includes what the processor wrote, which
> the editor's copy misses with the window closed), while AAX, whose processor lives in the algorithm, reads the
> editor link's published copy; AAX keeps a knob mirror and caches its chunk under a revision key, and AUv2 guards
> the base class's current preset. Written 2026-09-25; revised 2026-10-22 after reviewing the client plug-ins
> (Doctor Vibe, Fuzz Droid, Galaxy Brain, Hyper Boost, Scissor Hands in `../all_plugins`) and every preset path.
> Settled: optional-address `Action_start`/`Action_end` as the one transaction vocabulary, a step amendable until
> the end of its frame (clients just append linked edits), no `sort()`, observers replaced by a read-only frame
> value, preset features left to the client with the framework reporting each load, backward-compatible sessions.
>
> **Principle.** The framework owns **state and its mechanics**: storage, transport, persistence, restoration
> (any format, any thread, window open or closed), undo grouping and replay, and protocol correctness a client
> can't get right alone (host touches, size limits, chunk layouts, thread safety). It reports **what happened**
> (a load, where it came from, what a frame's edits resolved to), never **what it means**. The client owns
> **features** built on state: preset names, the modified marker, restore-to-preset, A/B, auto gain and other
> linked controls, layout tabs and orientation.
>
> **Test:** if a second client could reasonably want different behaviour, it's a feature, and the framework's
> job is the smallest tool that makes it correct.

## Where state lives today

| State | Owner | Persisted by | Undoable | Readable off main |
|---|---|---|---|---|
| Parameters | framework | framework | yes | yes |
| Document (`state::Model`) | framework | framework (state record) | yes | no: `Editor_link::view()` is main-only |
| Editor state (`State_map`) | **author**, via `save_state` / `load_state` | framework calls the author | no | no: author code |
| Window size | framework (`editor-width/height` in the map, `_last_size` per wrapper) | framework | no | main-only |
| Host events | pushed to the author via `notify(Host_event)` | — | — | — |

The author-owned rows cause four problems:

1. **Polling.** Hosts save and load from any thread (AAX by default, AUv3 over XPC, some AUv2 hosts). The
   framework can't call author code off main, so AAX, AUv2 and AUv3 each keep a `State_image`
   ([state_image.hpp](../libs/tiny_core/include/tiny_core/state_image.hpp)) that main rebuilds when dirty and
   otherwise once a second, only because editor state has no change signal.
2. **`notify` with the window closed.** `Host_preset_loaded` and `Dark_mode_changed` arrive synchronously,
   editor open or not. Authors must not touch live view resources there, a rule kept only by documentation. The
   clients show the opposite failure: their `notify` needs the GUI's environment, so it does nothing before the
   window first opens.
3. **Undo carriers disguised as parameters.** Editor state isn't undoable, so the clients encode undoable
   state in `Interface` parameters, and `Host_preset_loaded::add_param` exists only for that trick.
4. **Two serialisations per format** of the same `State_map`, hand-written in five wrappers and again in both
   preset exporters. No demo uses it: every demo's `save_state` / `load_state` is a stub, and every `notify`
   only tracks dark mode.

## Client review

The five products share one editor; their `notify`, `save_state` and `load_state` are identical.

**Their features, and how they're built today:**

- **Preset name.** `glb_preset_index` (Int 0…255, `Interface`) is a counter. A preset load bumps it,
  `Preset_history[index]` remembers the name and values, and undo moves the counter back, so the displayed
  name follows. A host load bumps it in `notify` and folds it into the load step with `add_param`. The name is
  saved as `"preset-name"` in editor state, and factory JSON presets embed it
  (`"editor": {"preset-name": "9000 Series"}`), which the exporters copy into `.tfx` and `.vstpreset`.
- **Modified marker.** `Preset_history` is an action-queue observer: it records which parameters the frame's
  edits moved and appends `*`. It must run at the end of the frame, after undo's replay is appended and the
  sets have landed, both to see undo and to read the post-frame preset counter.
- **Restore to preset value.** `Value_restorer` keeps 256 copies of preset values, indexed by the counter.
- **A/B.** `glb_a_settings` (`Interface` bool) brackets the swap's `Action_start`…`Action_end`, so the swap is
  one step and the selection follows undo. The slot contents live in `Ab_settings` members.
- **Linked controls.** `Auto_gain`, `mr::Table_link` and `ok_comp`'s `link_output` move a follower when a
  lead moves. Each copies the lead's start and end onto the follower and calls `Action_queue::sort()`, so the
  pair is one step.
- **Layout state.** `Layout_model`: the presets bank scope and first visible tab, the portrait panel, and the
  mod and delay panels (Galaxy Brain, Scissor Hands). It lives as long as the editor.
- **Window size.** The wrappers persist `_last_size`. The shared layout engine guesses whether the opening
  size was user-chosen by comparing it with `preferred_size()` (± the license banner strip), enforces a
  200×150 floor, and switches between landscape and portrait layouts by aspect ratio.

**Defects** (found by reading; not reproduced in a host). Each is a client feature failing for lack of a
framework tool:

1. **The preset name is lost on project reload.** The editor is built when the plug-in is created, but its
   GUI environment only on `on_gui_create`. A session load fires `notify` first, and it returns early: the
   index stays 0 and the editor shows "Default". A save made before the window ever opened writes no name.
   *Missing tool: a load hook that runs with the window closed, inside the load's step.*
2. **A save after undo writes the wrong name.** `save_state` writes the last captured name, not the name at
   the current index. *Missing tool: undoable, persisted view state.*
3. **`*` is unreliable.** After a reload the preset's values are captured from already-modified parameters,
   so the marker vanishes. Edits made with the window closed, or by automation, are never counted. *Missing
   tools: persisted view state; deriving from parameters instead of observing edits.*
4. **A/B doesn't persist.** A reload selects A and resets the inactive slot to defaults. Copy to the other
   slot isn't undoable. *Missing tool: view state.*
5. **Layout tabs don't persist** across project reloads. *Missing tool: view state.*
6. **Deep undo can show the wrong name.** The counter wraps after 256 loads. *Missing tool: view state.*

And one in the framework itself: **a gesture open when the view is torn down is never closed.** The `_gestured`
sets in VST3 and AAX only suppress the host echoing a value during a drag. So the host parameter stays touched,
and `Undo_history`'s `_active` stays raised: `can_undo()` is then false for the rest of the session.

What stays where it is: `Local_prefs` (per-user settings across instances), `Morph_data` (transient),
`glb_input` (`Interface`, a real DSP control deliberately unsaved), the `mph_*` morph targets (`Hidden`
parameters).

## Goals

- Framework-owned storage for everything the editor wants restored, behind a handle.
- Undo covering parameters, document, view state and (optionally) window size in **one** step per intent,
  with the grouping right for edits the client appends.
- Every load reported to the client, as data, inside its undo step, window open or closed.
- No `save_state`, `load_state` or `notify` on the editor.
- Saves buildable on demand from any thread, retiring `State_image` polling. Loads still apply on main.
- Window size framework-owned, its limits declared.
- Old sessions and presets keep loading.

Non-goal: the editor's transient caches (hover, animation, layout) stay editor members.

## Design

### 1. Edits and undo

Most of the undo machinery exists. `Undo_step` already holds parameter changes plus a state change, and replay
handles both (host loads use it through `amend_host_state`). What's added:

**One vocabulary: `Action_start` / `Action_end` with an optional address.**

```cpp
struct Action_start { std::optional<uint32_t> address{}; };
struct Action_end   { std::optional<uint32_t> address{}; };
```

- **With an address**, it's today's gesture: the host touches that parameter now and releases it at its own
  end. A press-and-hold takes over a touch-mode lane, and one lifted finger releases its knob while another is
  still held.
- **Without an address**, it's a transaction: each parameter it sets is touched just before its first
  `Set_param`, and released when the outermost start/end closes.
- **Undo** counts both in the one `_active` counter. Everything inside the outermost start/end is one step;
  two fingers on two knobs is already one step today, and stays so.
- **Code that pushes actions is unchanged:** `Action_start{addr}` still compiles. Code that reads `.address`
  must handle the empty case, and the compiler finds all of it: the wrappers, `Undo_history`, and in the
  clients `auto_gain.cpp`, `table_link.hpp` and `ok_comp`'s editor.

**Normalized once, in `run_frame`**, before anything reads the frame's actions, with an open-gesture count and
a per-parameter touch count:

```
bare set:     Set{B}                         →  Start{B} Set{B} End{B}            (own step)
transaction:  Start{} Set{A} Set{B} Set{A} End{}
          →   Start{} Start{A} Set{A} Start{B} Set{B} Set{A} End{A} End{B} End{}
inside {A}:   Start{A} Set{B} … End{A}       →  B touched before its set, released when the count hits 0
```

- Wrappers only ever see addressed, balanced starts and ends, and ignore the unaddressed pair. Their handlers
  barely change; the `_gestured` bookkeeping moves into the shared code.
- A parameter is touched once and released once, whichever mix of gestures touched it.
- `Undo_history` reads the normalized stream, so every set it sees has an addressed start before it.
- A bare set becoming its own step fixes today's silent case (not undoable, and VST3 `performEdit` without
  `beginEdit`). A drag that pushes only sets would become one step per frame, so drags still bracket themselves.
- **View teardown closes whatever is open:** an `End` for every touched address, and the open transaction, so
  the step records what landed. It also covers a client that calls `begin()` and never `end()`.

**A step stays open until the end of the frame it closed in.** Any later edit in that frame that begins and
finishes within it joins the step; a gesture that stays open past the frame opens its own. The action stream
is never reordered: this is undo bookkeeping only (one "closed this frame" flag on the last step, cleared each
frame), the same amend window host loads already use. The cost: an unrelated click in the same ~16 ms as
another gesture's end joins its step (two fingers only; harmless).

```
drag:      Start{drive} Set{drive}  +Set{output}                → inside drive's gesture: one step
click:     Start{drive} Set{drive} End{drive}  +Set{output}     → output folds into drive's step
legacy:    … End{drive}  +Start{output} Set{output} End{output} → one step
unrelated: End{A}  Start{B} Set{B} …(B continues)               → two steps
```

**Linked controls are client features; grouping is the framework's guarantee.** A client's link pass keeps
its rule, switch and suppression, and **appends** the follower's `Set_param`; the rules above put it in the
lead's gesture and step, with correct host touches. No framework link concept, and no positional insert:
the client never sees where its actions sit. `Action_queue::sort()` is removed: it was a workaround for the
missing guarantee, and it merged unrelated gestures that ended and began in one frame.

**Observers become a read-only frame value.** `install_observer` / `clear_observers` / `process_observers` are
replaced by `frame.edits()`: the frame's normalized actions, including undo/redo replay, available after the
frame resolves (the next frame reads the previous frame's). It's the same information observers had, without a
callback registry or install/clear lifecycle. A client feature that needs "what did the edits do" (a
last-touched display, analytics) reads it; the clients' modified marker no longer needs it (see appendix).

**`begin()` is a direct call as well as an action.** Document and view edits apply when made, while actions are
read at frame end. So `undo_redo.begin()` first records any uncommitted document or view edit as its own step,
then pushes `Action_start{}`. `end()` pushes `Action_end{}`; `transaction(fn)` wraps both for one frame.

**Several state sources fold into one step.** On close, the history asks each bound source to commit:

```cpp
class Undo_history {
public:
    enum class Source : std::uint8_t { Document, View, Size };
    enum class Commit : std::uint8_t { None, Recorded, Waiting };

    struct State_hooks {
        std::function<Commit()> commit{};
        std::function<void(const std::byte* base, const std::byte* next, std::size_t n)> replay{};
    };

    class Actor {
    public:
        auto undo() const -> void;
        auto redo() const -> void;
        auto can_undo() const -> bool;
        auto can_redo() const -> bool;
        auto begin() const -> void; // Everything until the matching end() is one step. Nests; spans frames.
        auto end() const -> void;
        template<typename F>
        auto transaction(F&& f) const -> void { begin(); f(); end(); }
    };

    auto bind_actions(Action_queue::Actor actions) -> void;
    auto bind_state(Source source, State_hooks hooks) -> void;
    auto process_actions(std::span<const User_action> normalized, Processor_state& state) -> void;
    auto perform_actions(Action_queue::Actor actions) -> void;
    auto end_frame() -> void; // Seals a step closed this frame.

    // A host load is a step like any other: params diffed here, each source records while it is open.
    auto begin_host_load(std::span<const double> before, std::span<const double> after) -> void;
    auto end_host_load() -> void;

    // From a source's commit: folds into the open step if there is one, else its own step.
    auto record_state(Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void;

private:
    struct State_change { Source source{}; std::vector<std::byte> from{}; std::vector<std::byte> to{}; };
    struct Undo_step { std::vector<Param_change> changes{}; std::vector<State_change> states{}; };
    struct Open_step { Active_map params{}; std::vector<State_change> states{}; std::uint32_t depth{}; std::uint32_t waiting{}; };

    std::optional<Open_step> _open{};
    std::optional<Open_step> _held{};  // Closed, but a source is still in flight; can_undo is false.
    bool _closed_this_frame{};         // The last step still accepts edits that complete this frame.
    // stacks, hooks, budget as today
};
```

- **Closing** (`depth` back to 0): collect parameter changes, then each source's commit. `Waiting` moves the
  step to `_held`, and that source's later `record_state` completes it.
- **The one timing case:** `Editor_side::commit` diffs against what the processor confirmed, so a
  processor-writing document can't record until its edit lands. The step is held (`can_undo` false, as
  during a gesture) for about one round trip. **While a step is held, that source keeps applying new edits
  locally but doesn't send them** until the held step lands, so a snapshot confirming both can't credit the
  next step's edits to the held one. Editor-only documents, view state and size never wait.
- **Replay** pushes parameter changes as actions, as today, then replays each `State_change` through its
  source's hook as a byte merge, so only the bytes the step changed revert. Undoing a preset load doesn't
  change your tab.
- `amend_host_load` and `amend_host_state` go away.

### 2. View state behind a handle

Author-declared, for anything the client wants restored or undone: `models/view.hpp`, with two parts:

- **Undoable**, follows undo: "what I did".
- **Restored only**, never touched by undo: "where I was looking".

What goes in it is the client's business. The clients' version, for illustration:

```cpp
struct View {
    struct Undoable {
        Fixed_string<64> preset_name{};
        std::array<float, num_params> preset_values{}; // basis for `*` and restore-to-preset
        bool b_selected{};
        std::array<float, num_params> other_slot{};    // the inactive A/B slot, knob space
    } undoable{};
    struct Restored {
        Preset_scope presets{};                        // kind, Fixed_string bank_id, first tab
        uint8_t portrait_panel{}, mod_panel{}, delay_panel{};
    } restored{};
    static constexpr auto version = uint32_t{1};

    // Optional author save/load, as the state record allows: raw bytes by default, or e.g. only the
    // entries of preset_values that differ from the session's parameters.

    // Optional, pure, main thread, inside every load's undo step (section 3).
    static auto on_host_load(View& v, std::span<const double> params, const Load_info& info) -> void;
};
```

- **Mechanism:** `Undoable` is a `state::Model` that only the editor writes and that has no processor side: the
  existing byte merge, record and replay, with no wire. `Restored` is a plain struct with a record, no undo.
  (Considered and dropped: widening `state::Model` itself; a framework-owned string map.)
- **Handle:** `edit.view.get()`, `edit.view.edit(fn)` for the undoable part (the document's verb: undo needs
  a step boundary), `edit.view.restored()` returning `Restored&`, compared with `memcmp` at frame end, which
  is also the publish point for the off-main copy, and `generation()`.
- **Storage** is wrapper-owned and lives as long as the plug-in, so a load with the window closed just writes
  it.
- **Fixed capacity:** strings are `Fixed_string<N>`. Anything dynamic belongs in the document with an author
  record, or in the buffer system ([buffer-system.md](buffer-system.md)).

### 3. Loads: reported, not interpreted

Every load reaches the client as data, inside the load's undo step, on main, window open or closed:

```cpp
struct Load_info {
    enum class Kind : std::uint8_t { Session, Preset };
    Kind kind{};
    std::string_view name{};        // A preset's name, when the load path knows it.
    std::string_view id{};          // A preset's id, when its file carries one.
    const State_map* legacy{};      // The editor map of a session saved before view state existed.
    bool view_restored{};           // The data carried a view record.
};
```

- **Session or preset:** the framework knows from the path. JSON preset loads (AU factory presets, AUv3
  presets, CLAP preset loads, web, the editor's own apply) are presets, and so is native data carrying the
  exporter's preset key. Everything else is a session. Host-saved presets (`.aupreset`, user `.tfx`, host
  `.vstpreset`) are full sessions and restore the view as saved, which is the right result.
- **The name** comes from the preset's `"name"` key, else its file name, else the factory list's name, else the
  exporter's key. It's metadata about the file, not client state: the framework carries it, the client decides
  whether it means anything.
- **Order of a load:** `begin_host_load(before, after)` with parameters applied; the document from its record;
  the view record, or defaults; `View::on_host_load(view, params, info)`; `end_host_load()`. The window size is
  applied at next open, or now if open, and is never part of the step.
- **Editor-side apply:** `edit.presets.apply(json)` applies a preset's parameters and document. Called inside the
  client's own transaction, next to its view edit, a preset loaded from the editor undoes exactly like one loaded
  by the host.

### 4. Immediate mode: drop `notify`

- `Dark_mode_changed` → `frame.appearance.dark`, read every frame (the clients' scheme setting reads it
  instead of caching `_system_dark`).
- `Host_preset_loaded` → parameters are per-frame; view state comes back through the load and `on_host_load`.
  An editor that needs an edge compares `frame.host.loads`.
- With the window closed, nothing happens until the next frame. What must react to a load does so in
  `on_host_load`, as data, not editor code.

### 5. Window size

Size storage and the host size protocol are the framework's. Layout variants (landscape/portrait), the banner
strip and how the editor fills the window stay the client's. The default and limits are declared:

```cpp
static constexpr auto sizing() -> Sizing
{
    return {.preferred = {776, 512}, .min = {200, 150}, .undoable = true}; // optional .max, .aspect
}
```

- **Declared limits:** the framework clamps in each format's own size check (CLAP `adjust_size`, VST3
  `checkSizeConstraint`, AUv2) instead of each editor enforcing a floor.
- **The restored size is handed over, not guessed.** That retires the clients' layout engine's "is the opening
  size preferred ± banner?" heuristic and its `last_size() != preferred` test.
- **Undoable resizes (opt-in):** an editor resize handle brackets its `Request_resize` stream with
  `begin()`/`end()`, so one drag is one step (`Source::Size`), and a resize can join a larger transaction like
  any other source. Undo sends the old size to the host; with the window closed it changes the stored size
  only. If the host clamps, the stored size follows what it applied.
- **Host-driven resizes** (the host's window frame, a host clamp) have no gesture boundary: they update the
  stored size without a step.

### 6. Saves from any thread, without polling

Once everything is framework-owned, a save is built on demand on the calling thread:

- **Parameters:** readable off main already.
- **Document:** `Editor_link` publishes a copy readable off main (seqlocked or mutex-guarded), updated when its
  view changes. Not the processor's copy: it lags the editor.
- **View and size:** framework storage copied under a small lock, published at frame end and on load.
- **Formats' own base state:** AUv3's `[super fullState]` and AUv2's `Super::SaveState`; verify each is safe off
  main before relying on it.

`State_image` keeps only its pending-load half. **Loads still apply on main.** The AUv2 and AUv3 repeating
relays and the 1 s poll go away.

### 7. Persistence and compatibility

**What goes where:**

| State | Session | Preset file | Undoable |
|---|---|---|---|
| Parameters | yes | yes (`params`) | yes |
| Document | yes | yes (`state`, base64) | yes |
| View, undoable part | yes | no | yes |
| View, restored part | yes | no | no |
| Preset name / id (file metadata) | no | yes | — |
| Window size | yes | no | opt-in |
| Host bypass | yes | no (`no_value`) | no: the host's control |

A preset file's own data is its parameters, its document and its name; anything a client derives from a preset
load (a displayed name, the values behind `*`) lives in the client's view state and is saved with the session.

**Per format,** new data is added the way the document record was:
- **Positional (CLAP stream, VST3 chunks):** appended last, as framed records (`Record_header` has a `kind`),
  "so older builds stop before it". `decode_record` already ignores trailing bytes.
- **Dictionary (AUv2, AUv3, AAX):** new framework keys (`tinyplug-view`, `tinyplug-size`, and for exporter
  output `tinyplug-preset-name` / `-id`).
- **JSON:** presets add `"name"` and optionally `"id"` (readable, Python-writable); web sessions add `"view"` as a
  base64 record.

**Old sessions in new builds:**
- Parameters and the document record are unchanged.
- **Every wrapper keeps its `State_map` reader indefinitely**, since shipped sessions contain it. The framework
  reads only its own size keys; the rest reaches the client untouched as `Load_info::legacy`.
- A missing view record loads as defaults, then `on_host_load`.
- The marker parameters were `Interface`, never persisted, so there's nothing to migrate.

**New sessions in old builds (downgrade):** older builds stop before appended records and ignore new keys.
Keep writing the two size keys into the legacy map so an older build opens at the right size.

**New plug-ins** never see `State_map`: it becomes an internal type for decoding old sessions. Positional
formats keep the map's slot with a count of 0.

### 8. Presets and tools

**Surfaces:**

| Surface | File | Loaded through |
|---|---|---|
| Author JSON (factory, user, the editor's own browser) | `.<ext>` | `State_adapter` |
| AUv2 / AUv3 factory presets, CLAP preset discovery, web | JSON | `State_adapter` → wrapper restore |
| AAX factory presets | `.tfx`, from `tfx_exporter` | `SetChunk` |
| VST3 factory presets | `.vstpreset`, from `vstpreset_exporter` | `setState` × 2 |
| Host-saved presets (`.aupreset`, user `.tfx`, host `.vstpreset`) | host's own | full session |

**The exporters are current today but fragile.** Both match the wrappers field by field (VST3: header,
knob-space floats, `no_value` bypass placeholder, record, then the controller's tagged pairs; AAX: num-params,
edit-keys, plain values by AAX ID, editor keys, base64 record). The shipped Doctor Vibe `.tfx` files are bare
chunks with `fSize` equal to the file size. But both re-implement each format's layout by hand, and nothing
tests them.
- **Shared chunk writers:** each format's session writer moves into a header that the wrapper and its exporter
  both call, so a layout exists once. The exporters write the preset key from the JSON's `"name"` / `"id"` or the
  file name, and pass any legacy `editor` map through while old clients need it.
- **Round-trip tests:** export a preset, load it in the fake VST3 and AAX hosts, compare parameters, document and
  `Load_info`.
- Minor: the AAX exporter's `Double` case falls through into `String`.

**JSON keys:** `version`, `params` (plain), `state` (base64 document), `name`, `id`. `editor` stays readable for
old files. `State_adapter::preset_state(extras)` loses its editor-map argument; `param_values(json)` stays.

## Migration

- **Demos:** delete the `save_state` / `load_state` / `notify` stubs, read dark mode from the frame,
  `preferred_size()` becomes `sizing()`, `Action_start` readers handle the empty address.
- **Clients** (a big job, sequenced separately), each feature rebuilt on the tools:
  - **Presets:** `preset_name` and `preset_values` in `View::Undoable`, set by `on_host_load` and inside the
    browser's transaction; `*` and restore-to-preset read them. Delete `Preset_history` (and its observer),
    `Value_restorer`, the counter logic and `add_param`.
  - **A/B:** slots and selection in `View::Undoable`, swaps in a transaction. Delete
    `Ab_settings::start/end_transaction`.
  - **Linked controls:** `Auto_gain`, `Table_link` and `link_output` append only the follower's `Set_param`.
    Dropping the copied start/end and `sort()` is optional: the copied form still groups correctly.
  - **Layout:** `Layout_model` tabs and panels into `View::Restored`; the layout engine takes its restored size
    and limits from `sizing()` and keeps its own landscape/portrait choice.
  - **Legacy:** `on_host_load` maps `Load_info::legacy`'s `"preset-name"`, and seeds `other_slot` from the loaded
    parameters for sessions without a view record.
  - **Leave `glb_preset_index` and `glb_a_settings` as `Interface`, unused.** Don't retire them with `Hidden`:
    `Hidden` persists, so they would start saving meaningless values.
  - Factory JSON gains `"name"` (or relies on the file name); `editor.preset-name` stays readable.
- A `MIGRATION.md` section, and CLAUDE.md's "Host-initiated preset/state loads" section rewritten.

## Open questions

All four have a lean; none blocks step 0.

1. **The window closes mid-transaction.** *Lean:* view teardown closes everything open (section 1), recording
   what landed as one step.
2. **A new transaction while a step is held.** *Lean:* the waiting source holds its new edits locally until the
   held step lands (section 1), so attribution stays exact; only processor-writing documents ever wait.
3. **Can a size step join a transaction?** *Lean:* yes, with no special rule; size is a source like any other.
4. **Confirm the client defects in a real host.** *Lean:* by hand in Logic or Live with Doctor Vibe before the
   client migration (step 7); framework steps don't depend on it.

## Sequencing

0. **Safety net.** Saved-session fixtures from the current build per format (CLAP, VST3, AUv2, AUv3, AAX, web
   JSON), loaded by the fake hosts and compared; exporter round-trip tests.
1. **Edits and undo** (section 1): the optional address, normalization in `run_frame`, teardown closing, the
   frame amend window, `begin()`, steps spanning sources, `frame.edits()`; remove `sort()`.
2. **Loads reported** (section 3): `Load_info`, `edit.presets.apply`, preset keys in JSON and exporter output,
   shared chunk writers.
3. **View state** and `on_host_load` (section 2), with persistence (section 7), wrappers one at a time behind a
   feature switch.
4. **`sizing()`** and framework-owned size (section 5).
5. **Drop `notify`** (section 4), the editor's `save_state` / `load_state`, and observers.
6. **Off-main snapshots**, then retire `State_image` polling (section 6).
7. **Client migration.**

## Appendix: client features built on the tools

None of this is framework code; it's how the clients' features would read.

```cpp
// Preset browser: params and document (framework), name and values (client), one step.
edit.undo_redo.transaction([&] {
    edit.presets.apply(json);
    edit.view.edit([name, values = values_of(json)](View& v) {
        v.undoable.preset_name = name;
        v.undoable.preset_values = values;
    });
});

// The modified marker: derived each frame, nothing observed.
const auto& v = edit.view.get().undoable;
const auto modified = std::ranges::any_of(persistent_addrs, [&](auto a) {
    return std::abs(params[a] - v.preset_values[a]) > 1e-6;
});
draw_text(std::string{v.preset_name} + (modified ? "*" : ""));

// Restore one control to its preset value: an ordinary gesture.
edit.actions.push(Action_start{addr});
edit.actions.push(Set_param{addr, edit.view.get().undoable.preset_values[addr]});
edit.actions.push(Action_end{addr});

// Host load of a preset or session, window open or closed.
static auto on_host_load(View& v, std::span<const double> params, const Load_info& info) -> void
{
    if (info.kind == Load_info::Kind::Preset) {
        v.undoable.preset_name = info.name;
        v.undoable.preset_values = to_array(params);
    }
    else if (info.legacy) {
        v.undoable.preset_name = legacy_name(*info.legacy);
        v.undoable.preset_values = to_array(params);
        v.undoable.other_slot = to_array(params);
    }
}

// A/B select: params plus view, one step.
edit.undo_redo.transaction([&] {
    const auto incoming = edit.view.get().undoable.other_slot;
    for (auto a : ab_addrs) edit.actions.push(Set_param{a, incoming[a]}); // touched implicitly
    edit.view.edit([b, outgoing = to_array(params)](View& v) {
        v.undoable.b_selected = b;
        v.undoable.other_slot = outgoing;
    });
});

// Copy to the other slot: a view edit, its own step.
edit.view.edit([cur = to_array(params)](View& v) { v.undoable.other_slot = cur; });

// Auto gain: the client's rule; appending is enough.
for (const auto& action : edit.actions.get_actions()) {
    if (const auto* set = std::get_if<Set_param>(&action); set && is_lead(set->address)) {
        edit.actions.push(Set_param{follower(set->address), compensated(*set, state)});
    }
}

// A drag over frames touching a parameter and the document: one step.
if (press)   { edit.undo_redo.begin(); edit.actions.push(Action_start{depth}); }
if (drag)    { edit.state.edit(paint); edit.actions.push(Set_param{depth, d}); }
if (release) { edit.actions.push(Action_end{depth}); edit.undo_redo.end(); }

// Switch a tab: restored, never a step.
edit.view.restored().mod_panel = Mod_panel::env;

// Resize handle (sizing().undoable): one step per drag.
if (grab)    edit.undo_redo.begin();
if (drag)    edit.actions.push(Request_resize{w, h});
if (release) edit.undo_redo.end();
```

## Related

- [undo_history.hpp](../libs/tinyplug/include/tinyplug/undo_history.hpp): `Undo_step`, the open host-load step this generalizes.
- [action_queue.hpp](../libs/tinyplug/include/tinyplug/action_queue.hpp): observers and `sort()`, both going.
- [state_record.hpp](../libs/tiny_core/include/tiny_core/state_record.hpp): the record container every new record reuses.
- [state_adapter.hpp](../libs/tiny_core/include/tiny_core/state_adapter.hpp): JSON presets, `editor_size_state`.
- [tools/presets/](../tools/presets/): the native exporters.
- After step 6, `Relay`'s repeating mode has no Apple clients.
