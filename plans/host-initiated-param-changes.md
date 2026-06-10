# Expose host-initiated preset/state loads to the editor (with undo)

## Context

When a user loads a preset from the DAW, or edits a parameter through the DAW's
generic UI, tinyplug's editor currently has **no event** telling it this
happened. The editor is immediate-mode: every frame each format's `on_draw`
overwrites its local `_ui_params` from `Ui_receiver::get_param` (which reflects
whatever the host stored — see [clap_view.cpp:86](../formats/clap/source/clap_view.cpp#L86),
[vst3_view.cpp](../formats/vst3/source/vst3_view.cpp), etc.). So host changes are
silently *displayed* but produce no callback and never enter the undo/redo
stack ([undo_history.cpp](../shared/tinyplug/undo_history.cpp), which only observes
the editor's own `Action_start`/`Set_param`/`Action_end` stream).

### Feasibility findings (all five formats explored)

- **Preset / full-state loads are cleanly detectable in every format** via their
  dedicated restore entry points — this is the reliable, unambiguous signal and
  the case the user named (loading a preset in the DAW):
  - VST3: `Vst3_controller::setComponentState` ([vst3_controller.cpp:220](../formats/vst3/source/vst3_controller.cpp#L220))
  - CLAP: `stateLoad` / `presetLoadFromLocation` → `_update_state` ([clap_plugin.cpp:417](../formats/clap/source/clap_plugin.cpp#L417))
  - AUv2: `RestoreState` ([auv2_effect.cpp:593](../formats/auv2/source/auv2_effect.cpp#L593))
  - AUv3: `setFullState` ([auv3_AUAudioUnit.mm:725](../formats/auv3/source/extension/auv3_AUAudioUnit.mm#L725))
  - AAX: `SetChunk` ([aax_parameters.cpp:284](../formats/aax/source/aax_parameters.cpp#L284))
- **Generic-UI single-param edits vs. automation are NOT cleanly separable** on
  most formats (VST3 routes both through one `setParamNormalized`; confirmed via
  SDK docs). VST3 *does* have extra visibility (`IEditControllerHostEditing`,
  `IAutomationState`) and JUCE confirms the industry punts on source attribution
  beyond gestures + preset detection. **Out of scope for v1** — this plan does
  only preset/full-state load detection. (Recorded as a documented follow-up.)

### v1 goal

A preset/full-state load, while the editor is open, is:
1. surfaced to the editor through a new **opt-in** `on_host_event` callback, and
2. recorded as a **single coalesced undo step** capturing every parameter the
   load changed, so the user can undo/redo the preset load.

## Design

Detection is **format-agnostic**: every format already exposes current param
values through `Ui_receiver::get_param`, and every format has exactly one
state-restore entry point. We add a monotonic "state epoch" counter that each
restore site bumps, expose it through the receiver, and let the shared view loop
diff `get_param` against the previous frame's values whenever the epoch advances.

All restore entry points and the draw loop run on the same UI/main thread, so a
plain `std::atomic<uint32_t>` (relaxed) is sufficient and future-proof.

### New shared types — [shared/tinyplug/tiny_events.h](../shared/tinyplug/tiny_events.h)

```cpp
struct Changed_param { uint32_t address{}; double value{}; }; // knob space, post-load
struct Host_preset_loaded { std::span<const Changed_param> changes{}; }; // span valid only during dispatch
using Host_event = std::variant<Host_preset_loaded>; // variant so future host-event kinds slot in
```

Add `state_epoch` to `Ui_receiver` next to `get_param`:

```cpp
std::function<uint32_t()> state_epoch = [] { return 0u; };
```

### Opt-in editor callback (concept-detected, like worker replies)

Mirror the worker-reply pattern in [tiny_worker.h](../shared/tinyplug/tiny_worker.h)
(`Receives_worker_reply_to_editor` + `try_drain_worker_to_editor` template).
Add to a shared header (tiny_view.h, near `run_frame`):

```cpp
template<typename E>
concept Receives_host_event = requires (E e, const Host_event& ev) { e.on_host_event(ev); };

template<typename E> // must be a template so if constexpr discards the un-detected branch
auto try_dispatch_host_event(E* editor, const Host_event& ev) -> void
{
    if constexpr (Receives_host_event<E>) { editor->on_host_event(ev); }
}
```

Plugins that don't declare `on_host_event` compile unchanged (no-op).

### Shared detector — new `Host_event_tracker` in [tiny_view.h](../shared/tinyplug/tiny_view.h)

```cpp
struct Host_event_tracker {
    std::vector<double> prev{};        // last frame's param values (knob space)
    uint32_t last_epoch{};
    bool primed{false};

    template<typename E>
    auto detect_and_dispatch(std::span<const double> fresh, uint32_t epoch,
                             Undo_history& undo, E* editor) -> void;
};
```

`detect_and_dispatch` (called once per frame from `run_frame`):
1. If `!primed`: copy `fresh`→`prev`, `last_epoch = epoch`, `primed = true`,
   return. (Avoids a spurious event the first frame the GUI opens — covers
   loads that happened while the GUI was closed.)
2. If `epoch != last_epoch`: a load happened. Build `changes` = every `i` where
   `std::abs(fresh[i] - prev[i]) > eps` (small eps to absorb host round-tripping).
   If non-empty: `undo.push_step(...)` (one coalesced step, from=`prev[i]`,
   to=`fresh[i]`), then `try_dispatch_host_event(editor, Host_preset_loaded{changes})`.
   Set `last_epoch = epoch`.
3. Always `prev = fresh` at the end.

### Wire into the view loop — [tiny_view.h](../shared/tinyplug/tiny_view.h) `view_impl::run_frame`

`run_frame` already receives `_receiver`, `_undo_history`, the editor pointer
(`_custom_view`), and `_ui_params` (already pulled from `get_param` by each
format's `on_draw`). Add one parameter `Host_event_tracker& _host_events` and,
near the top (after meter drain, before building `state`/drawing), call:

```cpp
_host_events.detect_and_dispatch(_ui_params, _receiver.state_epoch(),
                                 _undo_history, _custom_view);
```

This keeps the per-format `on_draw` bodies almost untouched — they each gain one
member and pass it through.

### Undo integration — [undo_history.hpp](../shared/tinyplug/undo_history.hpp) / [.cpp](../shared/tinyplug/undo_history.cpp)

Add a public way to push a pre-built coalesced step (reuses the existing private
`Undo_step`/`Param_change` and the existing `apply<>` replay):

```cpp
struct External_change { uint32_t addr{}; double from{}; double to{}; };
auto push_step(std::span<const External_change> changes) -> void; // one Undo_step; clears _redo_stack
```

Guard: if `_current` has an outstanding editor gesture (`_active != 0`), skip
pushing (defensive; a preset load mid-gesture is pathological). Undo replay is
already handled — `apply<true>` ([undo_history.cpp:169](../shared/tinyplug/undo_history.cpp#L169))
pushes `Action_start`/`Set_param(from)`/`Action_end` into the action queue, which
the editor's `action_handler` already routes back to the host; redo replays `to`.
These replayed edits flow through `get_param` next frame **without** bumping the
epoch, so they are correctly treated as ordinary editor edits, not new host
events.

### Per-format changes (same pattern, five sites)

For each format: (1) add `std::atomic<uint32_t> _state_epoch{};`, (2) bump it
(`fetch_add(1, relaxed)`) at the end of the restore entry point, (3) add the
`state_epoch` lambda where the `Ui_receiver` is built, (4) give the view a
`Host_event_tracker` member and pass it to `run_frame`.

| Format | Bump site | Receiver-build site (add `state_epoch`) |
|---|---|---|
| VST3 | `setComponentState` ([vst3_controller.cpp:220](../formats/vst3/source/vst3_controller.cpp#L220)) | `createView` lambda ([vst3_controller.cpp:611](../formats/vst3/source/vst3_controller.cpp#L611)) — `[this]{ return _state_epoch.load(); }`; epoch lives on the controller, no view back-pointer needed |
| CLAP | end of `_update_state` ([clap_plugin.cpp:417](../formats/clap/source/clap_plugin.cpp#L417)) | `guiCreate` receiver ([clap_plugin.cpp:922](../formats/clap/source/clap_plugin.cpp#L922)) |
| AUv2 | end of `RestoreState` ([auv2_effect.cpp:593](../formats/auv2/source/auv2_effect.cpp#L593)) | receiver in [auv2_effect.h:242](../formats/auv2/source/auv2_effect.h#L242) |
| AUv3 | end of `setFullState` ([auv3_AUAudioUnit.mm:725](../formats/auv3/source/extension/auv3_AUAudioUnit.mm#L725)) | `makeReceiver` ([auv3_AUAudioUnit.mm:211](../formats/auv3/source/extension/auv3_AUAudioUnit.mm#L211)) |
| AAX | end of `SetChunk` ([aax_parameters.cpp:284](../formats/aax/source/aax_parameters.cpp#L284)) | receiver in [aax_gui.cpp:43](../formats/aax/source/aax_gui.cpp#L43) |

The view edits are one-liners each: `Clap_view`/`Vst3_view`/`Auv2_view`/
`Auv3_view`/`Aax_gui` get a `Host_event_tracker _host_events;` member and pass
it into their `run_frame` call.

### Demonstrate it

Add an `on_host_event` to the `automation_tester` demo editor
([plugins/automation_tester/source/plug_editor.h](../plugins/automation_tester/source/plug_editor.h)/.cpp)
— e.g. log/flash on `Host_preset_loaded` — to exercise the opt-in path. Leave
the other demos without it to prove the no-op concept detection still compiles.

## Limitations (document in AGENTS.md + the demo)

- **Params only.** Undo of a preset load restores parameter values, not the
  editor `State_map` delivered via `load_state` — consistent with the current
  undo scope. Note it.
- **GUI must be open** at load time to capture an undo step / fire the event;
  loads while the editor is closed are absorbed silently on next open (and the
  host's own undo still covers them).
- **Preset loads only in v1.** Generic-UI single-param edits and automation are
  not surfaced; source attribution (VST3 `IEditControllerHostEditing` /
  `IAutomationState`, per-format delivery-path heuristics) is a planned follow-up.

## Verification

1. **Build serially** (per AGENTS.md, no `--parallel`):
   `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTINY_DEPS_PATH=../tiny_deps && cmake --build build`.
   Confirm all five formats and every demo still compile (proves the opt-in
   concept detection is a true no-op for editors without `on_host_event`).
2. **CLAP / VST3** in a host (e.g. the clap/vst3 validators + a DAW):
   - Open the `automation_tester` editor, move a couple of params.
   - Load a factory/user preset from the DAW. Confirm `on_host_event`
     (`Host_preset_loaded`) fires with the changed params.
   - Press the host/plugin undo: params revert to their pre-load values; redo
     re-applies the preset's values. Confirm no spurious event when *automation*
     plays back (epoch unchanged) or when editing in the plugin's own UI.
3. **AUv2 / AUv3** (macOS, Xcode generator for AUv3): repeat the preset-load +
   undo check via `auval` / Logic / GarageBand.
4. **AAX**: repeat via Pro Tools preset (`.tfx`) load + compare/undo.
5. Confirm opening the editor *after* a preset was loaded with the GUI closed
   produces **no** undo step and **no** event (priming path).
