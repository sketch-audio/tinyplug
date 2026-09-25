# Plan: Unified edit model — the framework owns all plug-in state

> Status: **exploratory.** Written 2026-09-25. The principle is settled; the shape is not. Where
> the view state lives (a new view model, or the existing `state::Model`) is deliberately left
> open below.
>
> **Principle:** every piece of state a plug-in persists or edits is owned by the framework. The
> editor is a function of that state, drawn each frame; it has no save, load or notification hooks.
> This is one of the framework's core promises (the author declares, the framework stores,
> transports, persists and undoes), and today the editor's `State_map` is the one exception.

## Where state lives today

| State | Owner | Persisted by | Undoable | Readable off main |
|---|---|---|---|---|
| Parameters | framework | framework | yes | yes |
| Document (`state::Model`) | framework | framework (state record) | yes | no: `Editor_link::view()` is main-only |
| Editor state (`State_map`) | **author**, via `save_state` / `load_state` | framework calls the author | no | no: author code |
| Window size | framework (reserved `tinyplug-` keys) | framework | no | main-only today |
| Host events | pushed to the author via `notify(Host_event)` | — | — | — |

The author-owned row is the cause of four problems:

1. **Polling.** Hosts save and load from any thread (AAX by default, AUv3 over XPC, some AUv2
   hosts). The framework can't call author code off main, so AAX, AUv2 and AUv3 each keep a
   `State_image` ([state_image.hpp](../libs/tiny_core/include/tiny_core/state_image.hpp)) that main
   rebuilds when dirty, and otherwise once a second, only because editor state has no change signal.
   Every instance pays that forever, whether or not the host ever saves off main.
2. **`notify` with the window closed.** `Host_preset_loaded` and `Dark_mode_changed` arrive
   synchronously, editor open or not, so authors must remember not to touch live view resources in
   `notify`. It's a rule enforced only by documentation.
3. **Undo carriers disguised as parameters.** Editor state isn't undoable, so a client plug-in that
   wants its preset name to follow undo encodes it in parameters. Real example (the client plug-ins
   in `../all_plugins`):
   - `glb_preset_index` is an `Interface` parameter used as a counter. On a named preset load,
     `notify` bumps it, maps the new index to the name in `Preset_history`, and folds the bump into
     the load's undo step with `Host_preset_loaded::add_param`. Undo moves the counter back and the
     displayed name follows.
   - `glb_a_settings` is an `Interface` bool flipped inside the same `Action_start`…`Action_end`
     transaction as the A/B value swap.

   Both work, but they spend parameter addresses, `au_order` entries and page-table slots on things
   that aren't parameters, and the counter needs `add_param`, which exists for this trick alone.
4. **Two serialisations per format** of the same `State_map`, hand-written in five wrappers.

## Goals

- Framework-owned storage for everything the editor wants restored, behind a handle.
- Undo that can cover parameters and non-parameter state in **one** step.
- No `save_state`, `load_state` or `notify` on the editor concept.
- Saves buildable on demand from any thread, which retires `State_image`'s polling. Loads still
  apply on main.

Non-goals: the editor's own transient caches (hover, animation, layout) stay editor members; only
what should survive a reload or follow undo moves.

## Design pieces

### 1. View state behind a handle

Storage is wrapper-owned and lives as long as the plug-in (like `Editor_link`), so a load with the
window closed just writes it. The editor gets a handle each frame:

```cpp
auto Editor::on_gui_draw(Frame& frame) -> void
{
    draw_label(frame.view.get().undoable.preset_name);

    if (const auto picked = preset_menu()) {
        frame.edit.transaction([&] {
            apply_preset_values(*picked);
            frame.view.set([&](auto& v) { v.undoable.preset_name = names[*picked]; });
        });
    }
}
```

Handle methods: `get()`, `set(fn)`, `generation()`. They're named methods, following the
`Actor`/`View` pattern. `generation()` lets an editor that derives something from view state notice
a change, as `blocks::View::fresh` does.

**Change detection, two options:**
- `set(fn)` is the signal: explicit, and costs nothing.
- Hand out `T&` and compare with `memcmp` at frame end against last frame's copy. For a small
  trivially copyable struct this is effectively free, and the author can't forget anything. It's
  the most immediate-mode of the two. (Preferred if the storage is typed.)

### 2. Where the view state lives — open

The user wants two kinds of view state:

- **Undoable, follows undo:** which preset is loaded, which A/B slot. "What I did."
- **Restored only, never touched by undo:** tab, zoom, scroll position. "Where I was looking." The
  window size already works this way.

**Option A — a new model, `models/view.hpp`, with two parts:**
```cpp
struct View {
    struct Undoable { Fixed_string<64> preset_name{}; uint8_t ab_slot{}; } undoable{};
    struct Restored { uint32_t tab{}; float zoom{1}; } restored{};
    static constexpr auto version = uint32_t{1};
    // Optional author save/load for migration; raw bytes otherwise, as the state record does.
};
```
It reads well for authors, keeps view state in one file, and is never sent to the processor. It's
built on the existing record, undo and byte-merge machinery.

**Option B — extend `state::Model`.** `Undoable` is structurally a document that never reaches the
processor. Give `state::Model` an editor-only scope (no transport, and the processor-side `Store`
compiles out), and keep only `Restored` separate, either as a small model or as framework keys. This
is one mechanism instead of two, but view concerns spread over two places, and "document" gets
broader.

**Option C — keep the string-keyed map, framework-owned.** `frame.view.get("preset-name")`. Least
migration and free serialisation, but no types, and an undoable/restored split would need per-key
flags.

Either way, strings need fixed capacity in a trivially copyable struct (`Fixed_string<N>`); fine for
names and short paths. Anything that's really dynamic belongs in the document with an author record
`save`/`load`, or in the buffer system ([buffer-system.md](buffer-system.md)).

### 3. Undo across parameters and state

Today `Undo_history::record_state` makes a document change its own step, "never folded into a
param gesture", and only host loads fold state in (`amend_host_state`). The editor needs a
transaction:

```cpp
frame.edit.transaction([&] { /* Set_param gestures, document or view edits */ });
```

That produces one undo step holding parameter changes plus a `{from, to}` for each state block
touched, and undo/redo replays both. This is the one real piece of new framework machinery here,
and it's needed whichever option wins section 2. Host loads keep their single step: the view's
undoable part folds in the same way the document does now, so undoing a load reverts values and
preset name together, with no counter and no `add_param`.

Undo replay touches only undoable bytes: undoing a preset load doesn't change your tab.

**The "modified" marker** (the asterisk after a preset name): derive it by comparing current
parameters with the values captured when the preset loaded; don't store it. A stored flag would
have to fold into every parameter gesture's undo step.

### 4. Immediate mode: drop `notify`

Everything `notify` carries becomes frame state:

- `Dark_mode_changed` → `frame.appearance.dark`, read every frame.
- `Host_preset_loaded` → parameters are already per-frame; preset name and the like come back
  through the view state the load restored; an editor that needs an edge compares a counter
  (`frame.host.loads`).
- With the window closed, nothing happens until the next frame. That removes the class of bugs
  where `notify` touches live view resources.

What's lost: `add_param` (unneeded once the name is state), and a synchronous hook at load time.
Anything that really must react to a load with the window closed (starting a fetch, say) would need
a framework-level hook, not an editor one. None is known today.

### 5. Saves from any thread, without polling

A save covers parameters (readable off main already), the document, view state and window size.
Once all four are framework-owned and readable off main, a save is built on demand on the calling
thread:

- **Document:** `Editor_link` publishes a copy readable off main, for example a seqlocked or
  mutex-guarded snapshot updated whenever the view changes. The processor copy isn't the right
  source: it lags the editor.
- **View state:** framework storage copied under a small lock.
- **Formats' own base state:** AUv3's `[super fullState]` (parameter tree, thread-safe) and AUv2's
  `Super::SaveState` (reads `Globals()`). Verify each is safe off main before relying on it.

`State_image` then keeps only its pending-load half. **Loads still apply on main**: they record an
undo step and must not race the editor. That's already how off-main loads work today.

The AUv2 and AUv3 `repeating` relays and the 1 s poll then go away, and so does the review's
"400 builds a second with 100 instances".

### 6. Persistence

- The view record goes where the editor `State_map` goes today: the VST3 controller chunk (the
  editor-side chunk of the dual-component pattern), the AUv2/AUv3 keys, the AAX chunk (base64), the
  CLAP stream, and preset JSON.
- **Presets vs sessions:** a preset file should carry the undoable part (the name) but not the
  restored-only part (the tab). Our preset JSON can choose. Host-driven saves (AU ClassInfo, VST3
  `.vstpreset`) can't tell a preset from a session, so those restore the tab too. That's minor, and
  already true today.
- Versioning and migration reuse the state record's container: author `save`/`load` optional, raw
  bytes by default.

## Migration

- Editors lose `save_state`, `load_state` and `notify`. For the demos that means deleting stubs and
  reading dark mode from the frame.
- Client plug-ins:
  - Move the preset name and A/B slot into view (or document) state.
  - Retire `glb_preset_index` and `glb_a_settings` with `Policy::Hidden`, keeping their addresses:
    permanence rules.
  - Map legacy sessions and presets whose name sits under `"editor"` → `"preset-name"` in the view
    record's `load`.
  - `Preset_history`'s index-to-name mapping and the `add_param` path go.
- A `MIGRATION.md` section, and CLAUDE.md's "Host-initiated preset/state loads" section rewritten.

## Open questions

1. Option A, B or C for view state (section 2).
2. `set(fn)` or `T&` plus frame-end `memcmp` for change detection.
3. The transaction API: a lambda scope as above, or explicit `begin`/`end` matching
   `Action_start`/`Action_end`. How a transaction interacts with gestures already in flight.
4. Whether `Restored` belongs in the undo-free part of the same model or in framework keys next to
   the window size.
5. Whether anything genuinely needs a synchronous load hook once `notify` is gone.

## Sequencing

1. The combined undo transaction (section 3): useful on its own, since the document can already use
   it.
2. The view state and handle (sections 1 and 2), with persistence (section 6), wrappers one at a
   time behind a feature switch.
3. Drop `notify` (section 4).
4. The document snapshot readable off main, then retire `State_image` polling (section 5).
5. Client migration.

## Related

- [state-persistence.md](state-persistence.md): the state record container this reuses.
- [relay-delivery.md](relay-delivery.md): the relay rework; after step 4 here, `repeating` has no
  Apple clients.
