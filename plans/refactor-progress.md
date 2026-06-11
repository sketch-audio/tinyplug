# Refactor progress (session log for handoff)

Working branch: **`next`**. Nothing committed this session — all changes are in
the working tree. This tracks the naming/namespace refactor (see
[structural-and-naming-refactor.md](structural-and-naming-refactor.md) for the
overall Phase 1/2 plan) plus a few new design docs.

## Build / validate (read first)

```
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DTINY_INSTALL_PLUGINS=OFF -DTINY_DEPS_PATH=../tiny_deps
cmake --build build-debug --parallel 8
```

- **Must pass `-DCMAKE_BUILD_TYPE`** — `tiny_deps` imported targets only set
  per-config `IMPORTED_LOCATION_<CFG>` (no config-agnostic fallback), so a
  type-less configure fails at generate with "IMPORTED_LOCATION not set".
- **`--parallel 8` is allowed now** (the old serial-only rule was lifted).
- **`build-debug` (Unix Makefiles) SKIPS AUv3** — AUv3 needs the Xcode generator:
  `cmake -S . -B build-macos -G Xcode -DTINY_DEPS_PATH=../tiny_deps`. ⚠️ **All
  AUv3 changes this session are UNVALIDATED** — do an Xcode build before relying
  on AUv3. (AUv3 `.mm` got real edits: `string_view` Cocoa wraps, item loop,
  namespace/file renames.)
- `template/` and `new_plugin.py` were **deleted** by the user (to be rewritten
  after the refactor) — out of scope; ignore.
- Downstream `~/Developer/hii` needs the same migrations — **not done**.

## What's DONE this session (all building green on the 4 Makefile formats + core lib)

1. **`.h` → `.hpp`** for all C++ headers. ObjC headers stay `.h` (the 4
   `@interface` headers + `TargetPlatforms.h`). Generated `*.h.in` → `*.hpp.in`
   except `app_info.h.in` (imported by a pure `.m`). Worker discovery is now
   `__has_include("worker.hpp")`. Script: [tools/rename_headers_to_hpp.sh](../tools/rename_headers_to_hpp.sh).

2. **User classes/files renamed + namespaced.** `param_model→params`,
   `meter_model→meters`, `plug_editor→editor`, `plug_processor→processor`,
   `plug_worker→worker`. Classes → `tiny::models::{Params,Meters}`,
   `tiny::plugin::{Processor,Editor,Worker}`; address enums → nested `Address`.
   Script: [tools/rename_user_classes.sh](../tools/rename_user_classes.sh).
   (`latency_demo` is the only dual-enum plug-in — `Address` aliases params,
   meter addresses qualified inline.)

3. **Meters fully migrated** → `tiny::meters`:
   - API: `make_specs() -> vector` replaced by per-address `make_spec(Address) -> meters::Spec`
     switch; `Spec` dropped its `address` field; registry builds a
     `std::array<Spec, num_meters>`.
   - Types: `Meter_policy→Policy` (members `Peak/Stream/Trig`), `Lin_range→Range`,
     `Meter_spec→Spec`, `Some_meter_model→Model`, `Meter_infos→Infos`.
   - `Tagged_meter` → **`tiny::view::Meter_state`** (moved into `tiny_view.hpp`,
     it's view-loop state not a model type).
   - Concept now requires a `Num_meters` sentinel.
   - The meter `plain_to_norm`/`norm_to_plain` moved into
     `formats/vst3/source/adapters.hpp` (VST3 is the only consumer).

4. **Format wrappers de-prefixed + per-format namespaces** (done format-by-format
   with a build after each):
   - Files dropped the format prefix (`vst3_controller.hpp→controller.hpp`, etc.).
     Special cases: `vst3_processor→audio_effect`, `auv3_AUAudioUnit→audio_unit`
     (file), `auv3_AUViewController→view_controller` (file).
   - Namespaces: `tiny::{aax,auv2,auv3,clap,vst3}`. Classes dropped prefixes:
     `vst3::{Audio_effect,Controller,View}`, `clap::{Plugin,View,Factory_presets,User_presets}`,
     `auv2::{Effect,View}`, `auv3::View`, `aax::{Parameters,Gui}`.
   - **ObjC class names kept** (`Auv3_AUAudioUnit`, `Auv3_AUViewController`) — only
     their files were renamed; ObjC class-name migration deferred.
   - Apple-template files kept as-is (`DSPKernel.hpp`, `AUProcessHelper.hpp`,
     `BufferedAudioBus.hpp`).
   - Generated headers de-prefixed: `aax_categories→categories`,
     `*_preset_list→preset_list`.
   - **Gotcha:** a format namespace can shadow an SDK namespace — `namespace
     tiny::clap` shadowed `::clap::helpers`; fixed by qualifying SDK refs as
     `::clap::helpers`. (VST3/AAX/AU SDKs use `Steinberg::`/`AAX_*`/`ausdk::`, no clash.)
   - **AUv2 entry point** changed: the AUSDK component factory is now
     `EffectFactory` (was `Auv2_effectFactory`, derived from the class name). The
     user updated `Info.plist` `factoryFunction` + `exports.txt` (`_EffectFactory`).
     Safe: AUs are identified by AudioComponent codes, not the factory symbol.

5. **Params migrated into `tiny::params`** (incremental; whole header wrapped, with
   a transitional `using` alias block in `namespace tiny` that shrinks each step):
   - Semantics: `*_semantics` → `params::Semantics::{Bool,List,Int,Fixed,Real}`,
     `Value_semantics → params::Semantics::Any`.
   - Adapters: `Adapt_*` → `params::Adapter::{Lin,Log,Pow,Taper,Piece}`,
     `Knob_adapter → params::Adapter::Any`, `Adapt_piece::Break_point →
     params::Adapter::Piece::Break_point`.
   - `Param_spec/Param_group/Param_node` → `params::Spec/Group/Node`.
   - `Host_policy → params::Policy` (members `Automation/Control/Hidden/Interface`).
   - `Units` members capitalized (`Generic/Percent/Decibels/Hertz/Milliseconds/Degrees`);
     type name still `Units` (still aliased).
   - `Param_infos → params::Infos`, `Some_param_model → params::Model`.
   - **Enum-case capitalization** is the new house style: client param/meter enum
     cases capitalized, sentinels `Num_params`/`Num_meters` (concepts require them).
     (User added the `Num_params` concept requirement + capitalized client enums.)
   - **`const char* → std::string_view`** for `Spec`/`Group` (`name`, `string_id`,
     `short_name`) and `Semantics::List::items` (`vector<string_view>`).
     Authoring unchanged (literals bind to `string_view`); consumers wrap in
     `std::string{…}.c_str()` at C-API boundaries. AAX `List` display-delegate
     materializes owning `std::string` copies (string_view::data() isn't
     NULL-terminated).

6. **MIGRATION.md** kept current — comprehensive downstream guide ([MIGRATION.md](../MIGRATION.md)).

## What REMAINS

### Params (finish the namespace pass)
Still reachable only via transitional `tiny::` aliases — migrate each into
`tiny::params` and delete its alias:
- `Host_formatter → Formatter`.
- `Param_order → Order`.
- `Units` (type kept; decide if it moves/renames).

### Value helpers consolidated — DONE this session
`Value_conv` + the conversion/query free functions were unified into one
`params::Value_helper` struct and **moved out of `tiny_params.hpp`** into new
[value_helper.hpp](value_helper.hpp) / [value_helper.cpp](value_helper.cpp)
(included via `tinyplug.hpp`), keeping the params header declarative.
- API map: `Value_conv::*` kept names; `plain_to_norm`/`norm_to_plain` →
  `plain_to_knob`/`knob_to_plain` (knob == norm); `get_{plain,host,knob}_default`
  → `default_value(spec, Space::*)`; `get_plain_min/max` → `plain_min/max`;
  `param_is_discrete`/`is_param_units` → `is_discrete`/`has_units`;
  `units_string` → `units_label`; `Value_space` → `Space`.
- **Detempled** `clamp`/`knob_next` to plain `double` (they were dead code).
- New: `convert(v, from, to, sem)`, `quantize(v, sem)` (the `norm(plain(…))`
  step-snap idiom, deduped from CLAP + the old fixed-quantize paths).
- `make_defaults` is now a free template `make_defaults<T, Infos>(space)` (was an
  `Infos` member); `make_array_by_indices` moved into `value_helper.hpp`.
- Includes evicted from `tiny_params.hpp`: `<format>` (was unused), `<cmath>`,
  `<functional>`, `<optional>`. The `constexpr` on the conversions was dropped
  (confirmed zero compile-time use) so the bodies could move to the `.cpp`.
- One deliberate behavior tweak: `default_value(Fixed, Host)` now quantizes (was
  raw def_val); identical for step-aligned defaults, which `validate_spec` requires.
- **Built green** on the 4 Makefile formats + core lib + 5 demo plug-ins.
  Unverified (same as always): AUv3 (needs Xcode gen) and the on-demand preset
  exporters (`presets/*_exporter.cpp` — mechanical `make_defaults` rename only).

### `Host_formatter` build-time cleanup (remaining)
- `Host_formatter`(`→Formatter`) impl already lives in `host_formatter.cpp`
  (`<sstream>`/`<iomanip>` are out of the headers). Renaming `Host_formatter →
  params::Formatter` and moving it under `params::` is the leftover step.

### Other framework namespaces (Phase 2, not started)
`tiny::events`, `tiny::view`, `tiny::edit`, `tiny::state`, `tiny::process`,
`tiny::task`, `tiny::worker`, `tiny::util` — same wrap-then-rename pattern.

### Worker `Model` restructure (not started)
Nested `user::Worker::Model` holding the four message aliases + constants;
`reply_capacity→outbound_capacity`, `poll_interval→update_period`.

### Platform library split — DONE this session
`shared/platform` is now its own static lib **`tiny_platform`** (was folded into
`tiny_shared_lib`). Dependency is strictly one-way `tiny_platform → tiny_shared_lib`.
- **Core is Skia-free** (verified: 0 skia in core's compile). Skia is **PRIVATE** on
  `tiny_platform` and explicit-`PRIVATE` on each plug-in lib (editor draws); frameworks
  stay PUBLIC (final-module symbol resolution). Redundant per-format `-framework Cocoa`
  removed (platform provides it).
- **OS macros**: `PLATFORM_*` → `TINY_PLATFORM_*`, the `Platform` struct removed (use
  `#if`), and the defs moved into core as `tinyplug/tiny_platform.hpp`. New umbrella
  `platform/platform.hpp`. `window_context.hpp` pimpl'd (no platform `#if`s, no Skia).
- **`win_view.cpp` → `win_dialogs.cpp`** split (the ~800-line dialog half), bridged by
  `win_internal.hpp` (`WM_TINY_SETCURSOR`, `view_window_class_name()`, dark-mode helpers).
- **Plug-in + format link lines reclassified**: plug-in lib = core PUBLIC + platform/skia
  PRIVATE; every format wrapper (incl. AUv3's 3 targets) links `tiny_platform` explicitly.
- **C4996**: `strncpy` → `memcpy`/`snprintf` (portable, null-terminating) in aax/clap.
- **Verified macOS Makefile build green.** iOS (Xcode) + Windows are user-verifying now;
  Windows `win_dialogs` already fixed a missing-`<sstream>` + dark-mode-helper round.

### Phase 1 structural (remaining)
Pitchfork layout (`include/`/`src/`/`wrappers/`/`examples/`/`libs/platform/`), PCH, unity
builds, CMakePresets, CI. (`tiny_platform` spin-out + Skia PRIVATE: done above.)

### Loose ends
- **Verify iOS (Xcode) + Windows** builds end-to-end (in progress).
- **Commit** the working tree on `next` (consider per-logical-step commits).
- Migrate downstream `~/Developer/hii` in lock-step (see MIGRATION.md §11 for the new
  link line + macro rename).
- Rebuild `template/` + `new_plugin.py` (deleted).
- Consider a `Value_helper` unit test (only untested logic-dense code; one behavior tweak).

## Established conventions (apply going forward)
- Headers `.hpp`; ObjC `.h`.
- Scoped-enum cases **Capitalized**; sentinels `Num_<things>`.
- Per-area namespaces under `tiny::` (`models`, `plugin`, `params`, `meters`,
  `view`, per-format `aax/auv2/auv3/clap/vst3`).
- Nested-type grouping pattern: `Semantics::{Bool,…}` + `Any`, `Adapter::{Lin,…}` + `Any`.
- Registry/concept naming mirrors across models: `params::Infos`/`params::Model`,
  `meters::Infos`/`meters::Model`.
- Migration mechanic: wrap header in target namespace → bare names inside, add
  `using` aliases in old location → migrate refs (internal bare, external
  qualified, `\b`-anchored perl) → delete alias → build. Keep MIGRATION.md current.

## Related design docs written this session (not implemented)
- [host-initiated-param-changes.md](host-initiated-param-changes.md) — surface
  host preset/state loads to the editor via opt-in `on_host_event` + coalesced undo.
- [asset-store.md](asset-store.md) — managed large-buffer resources
  (sampler/convolution/granular/looper); reshapes how `state-model`/`block-table` ship.
