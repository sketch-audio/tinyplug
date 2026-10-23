# State Demo

A sixteenth-note gate whose pattern is a **state document**: a plain struct the editor
authors, the processor reads every block, and the framework keeps in sync and undoable.

```cpp
struct State {
    static constexpr auto writers = state::Writers::Editor;
    std::array<std::uint8_t, 16> level{ /* percent per step */ };
};
```

| Area | What it is | Undo step |
|------|------------|-----------|
| grid | the pattern (state). Click or drag to paint levels; fast drags fill the columns between | one per stroke |
| strip | Depth, an ordinary parameter. Drag vertically | one per drag |
| ◀ ▶ | undo / redo, dimmed when there is nothing to walk to | — |
| PULSE, OFFBEAT, STUTTER | factory presets: Depth, the pattern and the preset's name. The current one is lit and named on the right | one per load |

The outlined column is the step the processor is playing. It follows the host transport
while it's moving, and free-runs at the host tempo otherwise.

## What to try

**Undo and redo across both kinds.** Paint a stroke, drag Depth, paint another stroke. Undo
three times: the second stroke, then Depth, then the first stroke come back in that order.
Redo walks forward again. One history covers parameters and the document.

**It's audible, not just drawn.** Put a sustained sound through it, paint a step to zero,
and hear the gap. Undo, and hear it come back.

**A stroke is one step.** Drag across all sixteen columns; one undo restores all of them.

**Close the window mid-stroke.** The stroke still becomes a step (`on_gui_hide` commits),
and undo still works after reopening, because the history and the document live with
the plug-in, not the window.

**Per format.** It behaves the same everywhere; only the wire differs. CLAP and the AUs
apply an edit on the next block. VST3 sends it as an `IMessage`, and AAX through Direct
Data within about one 30 ms wakeup.

**Save and reopen.** The pattern is stored with the session, and in presets, through the
`save`/`load` pair in `models/state.hpp`. Loading a session or preset is one undo step
for the params and the pattern together.

## Presets

`presets/*.json` is the source: `version`, `name`, `params` (plain values by keypath) and
`state` (the document's record, base64). From there:

- **AUv2, AUv3, CLAP** list the JSON as factory presets (`TINY_NATIVE_PRESETS_DIR`).
- **AAX and VST3** get `presets/out/*.tfx` and `*.vstpreset`, written by the exporters in
  `tools/presets` and checked in. AAX copies the `.tfx` into the bundle
  (`TINY_FORMAT_PRESETS_DIR`); a `.vstpreset` is placed by an installer. After changing a
  preset, build and run `StateDemo_tfx_exporter` and `StateDemo_vstpreset_exporter`.
- **The editor's buttons** load the same JSON, compiled in by `CMakeLists.txt`
  (`factory_presets.hpp`), through `Edit_context::apply_preset` inside a transaction that
  also sets the name.

The **current preset's name** is view state (`models/view.hpp`, `View::Undoable`): saved with
the session, and walked by undo. Load OFFBEAT, then STUTTER, then undo: the pattern, Depth
and the name all go back to OFFBEAT. A host's preset load names it too, through
`on_host_load`; a session load restores what was saved. Editing the pattern after a load
keeps the name: a modified marker would be the plug-in's own feature, built on the same
view state.
