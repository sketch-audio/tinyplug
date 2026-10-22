# Plan: Headless plug-ins — processor-only, no editor

> Status: **design; the compile-time half exists.** Lets an author ship a plug-in with **no editor**: the host
> shows its own generic parameter UI, and the binary doesn't link `tiny_platform` or Skia. What remains is the
> link-time half (CMake, per wrapper) and each format's "no custom view" answer.

## What exists

The model layer already treats the editor as optional at compile time, the same way as the worker:
`configure_plugin` ([helpers.cmake](../cmake/helpers.cmake)) runs `_tiny_optional(EDITOR editor.hpp edit::None
edit::Editor)`, and the generated `<tiny_plugin.hpp>` ([tiny_plugin.hpp.in](../cmake/tiny_plugin.hpp.in)) defines
`TINY_HAS_EDITOR`, `has_editor`, and `User_editor` as `edit::None` when the file is absent. Then
`configure_plugin` stops with *"editor.hpp is required (headless plug-ins are not supported yet)"*: no wrapper
gates its editor members or view sources on `TINY_HAS_EDITOR` yet.

The web format's processor module is already a headless build in practice: a static archive contributes only the
objects a module references, so it never links `editor.o` or Skia.

## Goal & shape

Two ways a plug-in becomes headless:

1. **No `editor.hpp`** (the worker-like trigger; detection already exists).
2. **`TINY_HEADLESS ON`** (target property / `-D`), forcing headless even when `editor.hpp` exists, to build an
   existing editor plug-in headless (see Scope below). `TINY_HAS_EDITOR = (EXISTS editor.hpp) AND NOT TINY_HEADLESS`.

CMake stays the single source of truth, because only it can also exclude the view sources and drop the platform
link. In code, the editor type is named only inside `#if TINY_HAS_EDITOR` regions in the wrappers, and
`if constexpr (has_editor)` inside templates (e.g. editor-side worker channels).

`run_frame` in `tiny_view.hpp` is a template over the view type, so it needs no change beyond not being
instantiated.

## Client API / authoring

- **Omit `editor.hpp` and `editor.cpp`.** That's the whole client-side change —
  exactly parallel to omitting `worker.hpp`. `processor.hpp` is unchanged;
  `process::Processor` remains the only required user class. Params/meters models still
  drive the host's generic UI.
- **Scaffold:** `tools/new_plugin.py` gains `--headless`, which omits the editor
  files and (optionally) sets `TINY_HEADLESS`. A headless template variant under
  `template/` ships without `editor.*`.
- **Force-headless an existing editor plug-in:** set `TINY_HEADLESS ON` (or
  `-DTINY_HEADLESS=ON`); the editor sources just don't compile/link.

Nothing the author writes references the editor, so there is no new runtime API —
headless is the *absence* of the editor surface.

## CMake changes (the invasive part)

Each `make_<format>_plugin.cmake` currently unconditionally: adds `view.*`
sources, links `${TINY_PLATFORM_LIB}`, and calls `configure_mac_view`
(compiles `mac_view.mm`). Gate all three on `TINY_HAS_EDITOR`.

1. **Detect** in `configure_plug_info` (or a new `detect_editor` helper,
   [cmake/helpers.cmake](../cmake/helpers.cmake)): compute `TINY_HAS_EDITOR` from
   `EXISTS <src>/editor.hpp` and the `TINY_HEADLESS` property; store it as a target
   property and a `target_compile_definitions(... TINY_HAS_EDITOR=0|1)`.
2. **Per-format conditional** in each `make_<format>_plugin.cmake`:

   ```cmake
   if(TINY_HAS_EDITOR)
       target_sources(${TGT} PRIVATE ${SOURCE_DIR}/source/view.cpp ${SOURCE_DIR}/source/view.hpp)
       target_link_libraries(${TGT} PRIVATE ${TINY_PLATFORM_LIB})   # → Skia
       if(APPLE)
           configure_mac_view(${TGT} ${TINY_BASE_FILENAME} ${TINY_VERSION_STRING} ${TINY_BUILD_NUMBER})
       endif()
   endif()
   ```

3. **Result:** a headless plug-in links neither `tiny_platform` nor Skia, compiles
   no `view.*` / `mac_view.mm`, and is dramatically smaller — the headline win.

`${TINY_DSP_LIB}` and the core `tiny::tinyplug` link unchanged. The processor,
params, meters, worker, state, and the new buffer/block plans are all unaffected.

## Per-format implementation details

Each wrapper keeps its processor/controller side intact and drops only the
editor-facing pieces. Always-compiled wrapper files that hold a
`User_editor` member gate it with `#if TINY_HAS_EDITOR`; the view-only source
files are excluded by CMake (above).

### VST3 — cleanest

Still two components; the controller persists (it owns params). Headless just
declines to make a view.

- [controller.cpp:613](../wrappers/vst3/source/controller.cpp) `createView` →
  `return nullptr` when `!TINY_HAS_EDITOR`.
- [controller.hpp:82](../wrappers/vst3/source/controller.hpp) `std::optional<User_editor> _editor` → `#if TINY_HAS_EDITOR`.
- `view.cpp/.hpp` excluded by CMake. The host (Reaper/Cubase) shows its generic
  parameter panel. No other change — `kDistributable`, state split, meters all
  unaffected.

### CLAP — cleanest

clap-helpers advertises the `gui` extension only if the plug-in opts in (same
pattern as the render extension).

- Make `implementsGui()` return `has_editor` (don't advertise `CLAP_EXT_GUI` when
  headless); the `guiCreate`/etc. overrides
  ([plugin.cpp:944](../wrappers/clap/source/plugin.cpp)) become unreachable.
- [plugin.hpp:161](../wrappers/clap/source/plugin.hpp) `_editor` → `#if TINY_HAS_EDITOR`; `view.cpp` excluded.

### AUv2 — drop the Cocoa view property

- [effect.cpp:88 / :129](../wrappers/auv2/source/effect.cpp): remove the
  `kAudioUnitProperty_CocoaUI` cases from `GetPropertyInfo`/`GetProperty` under
  `#if !TINY_HAS_EDITOR` (return the default "unsupported" so the host uses its
  generic AU view).
- [effect.hpp:131](../wrappers/auv2/source/effect.hpp) `_editor` gated;
  `view.cpp` + `view_factory.mm` excluded; `TINY_AUV2_VIEW_CLASS` / `configure_mac_view`
  skipped. Logic shows its generic knob panel.

### AUv3 — supported, via an empty view controller

AUv3 is UI-centric (extension + container app that hosts the view, + a shared
`*_core` framework on iOS), and some hosts expect a view controller to exist.
Rather than returning `nil` (which a few hosts handle poorly), **headless returns
a minimal empty `AUViewController`** — a real, valid controller backing a blank
(or a tiny "no UI" placeholder) view of `preferred_size`.

- Keep `requestViewControllerWithCompletionHandler:`
  ([audio_unit.mm](../wrappers/auv3/source/extension/audio_unit.mm)) returning a
  controller; under `#if !TINY_HAS_EDITOR` it instantiates a stock
  `AUViewController` with an empty `loadView` (no Skia surface, no `User_editor`),
  instead of the real `view_controller.mm`. This keeps the AU well-formed for every
  host while pulling in no platform/Skia dependency.
- `view.cpp` and the real `view_controller.mm` are excluded by CMake; a small
  headless view-controller source (no platform link) is compiled in their place.
  The `_editor` ivar is gated.
- **Container app:** its reason to exist is previewing the UI; headless ⇒ a thin
  app that just hosts the empty controller (or is skipped on desktop where the
  host provides generic UI). On **iOS** the empty controller is what the host
  shows — acceptable, just blank — so iOS headless is functional, if visually
  empty.

### AAX — no custom GUI, host-generated controls

- Exclude `gui.cpp/.hpp` ([make_aax_plugin.cmake:29](../wrappers/aax/make_aax_plugin.cmake));
  gate the `User_editor` construction in
  [parameters.hpp:27](../wrappers/aax/source/parameters.hpp). Pro Tools renders a
  generic plug-in panel from the parameters / **page tables**.
- **Page-table generation still runs** (param layout is independent of the GUI).
- **Risk to verify:** confirm AAX/Pro Tools accepts a plug-in that registers no
  `AAX_IEffectGUI` and auto-generates the panel (the describe must not require a
  view class). This is the least-certain format; validate early with the
  aax-validator and a real Pro Tools.

## Cross-cutting

- **Editor state (`State_map`).** Headless has no editor state. Each wrapper's
  state save/load should write/expect an empty editor map under `#if
  !TINY_HAS_EDITOR` (or skip the editor section). Backward compatible: a headless
  build loading an old session ignores the editor map; params restore normally.
- **Worker editor channels.** `From_editor` / `To_editor` are unused headless but
  harmless; the processor-side worker is unaffected. Optionally `#if`-gate the
  editor-side queues, but not required for correctness.
- **`Plugin_state` / meters.** Still produced for the host; the generic host UI
  consumes params, and meters simply have no editor reading them (fine).

## Scope decision: "load existing editor plug-ins headless"

Splitting the user's open question into the two things it could mean:

- **Build-time force-headless (in scope, cheap):** `TINY_HEADLESS ON` compiles an
  editor-bearing plug-in *as* headless — the editor sources don't compile/link.
  Delivered by the same `TINY_HAS_EDITOR` switch. Useful for shipping a
  no-UI/server/CI variant of a normal plug-in from one codebase.
- **Run-time headless of a shipped editor binary (out of scope — and unnecessary):**
  a binary already compiled *with* an editor doesn't need anything from us to run
  without UI — every host can simply not open the editor. There is no framework
  work to do here, so we don't.

So: the valuable interpretation is in; the complicated one needs no code.

## Limitations

- **AUv3** headless shows an empty view controller (blank panel) rather than the
  host's generic UI; functional on desktop and iOS but visually empty where the
  host renders the AU's own view.
- **AAX** no-GUI path needs host validation; it's the riskiest format.
- A headless plug-in relies on the host's generic parameter UI; quality/coverage
  of that UI varies by host (and AAX page-table layout still matters).

## Verification

1. **Headless template builds** with no `editor.*`, links neither `tiny_platform`
   nor Skia (check the link line), and is materially smaller than the same plug-in
   with an editor.
2. **Each format loads & shows generic UI:** VST3 in Reaper/Cubase, CLAP host,
   AUv2 in Logic, AAX in Pro Tools — params automate and persist; no view is
   offered; no crash on "open editor".
3. **Force-headless** an existing editor demo via `TINY_HEADLESS ON`; confirm it
   builds editor-free and behaves identically to (1).
4. **Editor build still works** unchanged (the default path) — `TINY_HAS_EDITOR=1`
   regression of an existing demo.
5. **Validators:** auval / pluginval / clap-validator / aax-validator pass for a
   headless build (especially AAX's no-GUI describe).
6. **Old-session compat:** load a session saved by the editor build into the
   headless build; params restore, editor map ignored, no crash.

## Key files

| File | Change |
|---|---|
| [libs/tinyplug/include/tinyplug/tiny_view.hpp](../libs/tinyplug/include/tinyplug/tiny_view.hpp) (or new `tiny_editor.hpp`) | `No_editor`, `User_editor`, `has_editor`, `TINY_HAS_EDITOR` discovery |
| [libs/tinyplug/include/tinyplug/tinyplug.hpp](../libs/tinyplug/include/tinyplug/tinyplug.hpp) | Include editor discovery (last, like the worker) |
| [cmake/helpers.cmake](../cmake/helpers.cmake) | Compute `TINY_HAS_EDITOR`; define the macro; gate `configure_mac_view` |
| [wrappers/clap/make_clap_plugin.cmake](../wrappers/clap/make_clap_plugin.cmake) + 4 others | Conditional `view.*` sources, `TINY_PLATFORM_LIB`, `configure_mac_view` |
| [wrappers/vst3/source/controller.cpp](../wrappers/vst3/source/controller.cpp) / `.hpp` | `createView`→nullptr; gate `_editor` |
| [wrappers/clap/source/plugin.cpp](../wrappers/clap/source/plugin.cpp) / `.hpp` | `implementsGui()`→`has_editor`; gate `_editor` |
| [wrappers/auv2/source/effect.cpp](../wrappers/auv2/source/effect.cpp) / `.hpp` | Drop `kAudioUnitProperty_CocoaUI`; gate `_editor` |
| [wrappers/auv3/source/extension/audio_unit.mm](../wrappers/auv3/source/extension/audio_unit.mm) + container app | Empty `AUViewController` (no platform link); gate `_editor` |
| [wrappers/aax/source/parameters.hpp](../wrappers/aax/source/parameters.hpp) + [make_aax_plugin.cmake](../wrappers/aax/make_aax_plugin.cmake) | Gate editor; exclude `gui.*`; verify no-GUI describe |
| [tools/new_plugin.py](../tools/new_plugin.py) + `template/` | `--headless` scaffold variant |
