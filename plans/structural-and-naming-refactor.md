# Plan: Tinyplug structural + API/naming refactor

> **Note:** This plan supersedes the prior worker-channel plan in this file. The worker work has already shipped (see the `worker` branch and the `v0.x` tag). This document covers a two-phase framework refactor on the `next` branch.

## Context

Tinyplug has reached the point where two parallel improvements are due:

1. **Structural / build conformance.** The repo's layout (`shared/`, `formats/`, `plugins/`) and CMake wiring don't follow common open-source C++ project conventions, and there's substantial build-time fat (Skia pulled PUBLIC into every wrapper, no PCH or unity builds, no presets). The platform layer is large (~3.7K LOC, depends on Skia + native OS frameworks) and conceptually distinct enough to live as its own library.

2. **API/naming refactor.** The flat `tiny::` namespace with ~150 public types named in `Capitalized_snake_case` with redundant prefixes (`Param_spec`, `Bool_semantics`, `Adapt_lin`, `Plug_processor`) is verbose. Grouping into sub-namespaces and nesting variant alternatives inside their parent struct gives terser, more navigable names without losing information.

**Sequencing:** Phase 1 (structural) lands first. Once the layout is settled and builds faster, Phase 2 (naming) is a more focused rename pass with cleaner targets.

This is a single multi-week effort on the `next` branch.

## Phase 1 — Structural refactor

### 1.1 Repo layout (Pitchfork-style)

Move from the current `shared/`, `formats/`, `plugins/` split to a more conventional layout:

```
tinyplug/
├── include/tinyplug/           public framework headers (was include/tinyplug/*.h, *.hpp)
│   └── detail/                  internal-but-header-only utilities
├── src/                         framework implementations (was include/tinyplug/*.cpp)
├── libs/
│   └── platform/                spun-out platform library (was shared/platform/)
│       ├── include/tinyplug/platform/
│       └── src/
├── wrappers/                    format wrappers (was formats/)
│   ├── aax/
│   ├── auv2/
│   ├── auv3/
│   ├── clap/
│   └── vst3/
├── examples/                    example plug-ins (was plugins/)
│   ├── gain_demo/
│   ├── automation_tester/
│   ├── latency_demo/
│   ├── platform_demo/
│   └── worker_demo/
├── template/                    new-plug-in scaffold
├── cmake/                       helpers, modules, configs
├── docs/                        ARCHITECTURE.md, CONTRIBUTING.md, Doxygen output
├── tools/                       new_plugin.py and utilities
├── plans/                       internal design docs (this file)
├── .github/workflows/           CI
├── .clang-format
├── .editorconfig
├── CMakePresets.json
├── CMakeLists.txt               (top level)
├── LICENSE
└── README.md
```

**Why:**
- `include/` vs `src/` separation makes the public-vs-internal API boundary explicit (Pitchfork convention).
- `examples/` is the canonical OSS name for demo code (Boost, Catch2, fmt, etc.).
- `wrappers/` is more descriptive than `formats/`.
- `libs/platform/` as a sibling makes the spin-out concrete and movable to its own repo later with minimal repointing.

Every `#include "tinyplug/..."` line in the codebase needs to be reviewed but most already use that prefix today. Wrapper code with relative includes like `#include "plug_processor.h"` continues to work — that's per-plug-in code resolved via the plug-in's own source dir.

### 1.2 Spin out `tiny_platform` as a separate library

Currently `shared/platform/` is built into `tiny_shared_lib`. The proposal:

- Promote it to `libs/platform/` with its own `CMakeLists.txt` producing the `tiny_platform` static library.
- It carries the Skia dependency (PUBLIC) and the native OS frameworks (Cocoa, Metal, UIKit, etc.).
- The core framework library (`tiny_shared_lib` — renamed to `tiny_core` in this phase) depends on `tiny_platform` only where it actually needs platform types. For the bulk of params/events/state/tasks code, no Skia dep is needed.

**Benefits:**
- Concretely modularizes the heaviest external dep into one place.
- Eventually extractable to its own GitHub repo if desired.
- Makes "what does tinyplug look like without the UI layer?" a real, testable question.

**Cost:**
- CMakeLists rework (one new file, edits to root + shared).
- Header path changes inside platform code: `#include "platform_view.h"` → `#include "tinyplug/platform/view.h"`.
- Wrappers re-link explicitly: they already implicitly get platform via `tiny_shared_lib`; that becomes explicit `tiny::core + tiny::platform`.

### 1.3 Decouple Skia from `tiny_core`'s PUBLIC linkage

Today `tiny_shared_lib` declares `tiny::skia` as `PUBLIC` — every wrapper transitively re-parses Skia headers even when it doesn't need them. Investigation step: which core headers actually reference Skia types? If only the view-side ones do, push Skia to `PRIVATE` on `tiny_core` and `PUBLIC` only on `tiny_platform`. Wrappers that need to render UI link both; wrappers that don't need to link only `tiny_core`.

Expected savings: **10-15% wrapper compile time** for any format that doesn't draw (AAX in some configs, future headless wrappers).

### 1.4 Build-time quick wins

| Win | Effort | Impact |
|---|---|---|
| **CMakePresets.json** (`default`, `debug`, `ios` presets) | 2h | UX (no more long `-D` flags); standardizes onboarding |
| **`.clang-format`** matching README's Stroustrup-lean style | 1h | Consistency; enables IDE auto-format |
| **`.editorconfig`** | 30min | Editor agreement on tabs/spaces, line endings |
| **PCH** on each wrapper for the SDK header (`<public.sdk/...>`, `<AAX_*.h>`, `<AudioUnitSDK/...>`, etc.) | 4-6h | 20-30% wrapper compile reduction |
| **Unity builds** per wrapper (`set_target_properties(... PROPERTIES UNITY_BUILD ON)`) | 2-3h | 15-25% wall-clock per wrapper |
| **Decouple Skia PUBLIC linkage** (see 1.3) | 4h | 10-15% additional |

Stacked, this puts wrapper rebuilds at roughly half the current time on a clean build.

### 1.5 CI

Add `.github/workflows/build.yml`:

- macOS x64 + arm64 matrix: configure + serial build of all targets (matches the existing "no `--parallel`" preference).
- Job cache: `tiny_deps`, CMake configure, ccache where available.
- Triggers: push to `main`/`next`, every PR.

Add `.github/workflows/lint.yml`:

- `clang-format --dry-run --Werror` check.

CI is the canary for any structural change that breaks one of the format builds.

### 1.6 What NOT to spin out

Considered and rejected for this phase:

| Candidate | Why not |
|---|---|
| `tiny_tasks` | Only ~600 LOC, deeply intertwined with the worker runner concept; cleaner kept in core. |
| `tiny_state` | ~400 LOC, tinyplug-specific contracts; not standalone-useful enough yet. |
| `tiny_dsp` | ~200 LOC of header-only helpers; too small to justify a library boundary. |
| `tiny_params` | Header-only type defs but tightly coupled to the registry/concept system in core. |
| `tiny_presets` | Tinyplug-specific; not portable to other frameworks. |

`tiny_platform` is the only spin-out that earns its keep on the user's "sufficient code AND unique functionality" criterion.

### 1.7 Phase 1 verification

1. **Full build green** after layout move (no source changes, just file/path moves + CMake adjustments).
2. **All 30 plug-in/format targets compile** (5 plug-ins × 4-5 formats).
3. **Skia decoupling test:** core compiles without Skia headers in include path; wrappers that link only core (none today, but `tiny::core` should not transitively pull Skia).
4. **Build-time measurement** (before/after, clean build of all targets, serial). Document in the PR description.
5. **CI pipeline green** on first PR.
6. **Worker_demo VST3 functional check** in Bitwig and Live (same as worker phase) — no regressions from the structural shuffle.

## Phase 2 — Namespace/naming refactor

The naming and nesting refactor as previously planned, but executed *after* Phase 1 lands so all file paths are already in their final locations.

### Phase 2 outcome

A plug-in author writes:

```cpp
using namespace tiny;

namespace tiny::models {
struct Params {
    enum class Address : uint32_t { gain, num_params };
    static auto build_tree() -> params::Node { /* ... */ }
};
}

namespace tiny::user {
class Processor {
public:
    auto reset(double sr) -> void;
    auto handle_event(const events::Render& e) -> void;
    auto process(process::Dsp& ctx) -> void;
    // ...
};
}
```

Reads cleaner, gives import grouping, and lays the groundwork for the forthcoming Block/Table/State models that need named-by-namespace homes.

### Phase 2 decisions baked in

- **Case style:** Keep current `Capitalized_snake_case` for type identifiers. Multi-word top-level names stay as-is (`Set_param`, `Action_start`, `Request_resize`, `Param_node`). Single-word nested names use just the word (`Lin`, `Bool`, `Down`).
- **Drop redundant prefixes/suffixes** when nesting absorbs the context: `Adapt_lin` → `Adapter::Lin`, `Bool_semantics` → `Semantics::Bool`, `Param_spec` → `params::Spec`, `Render_event` → `events::Render`. But preserve disambiguating words when needed: `events::Set_param` and `events::Set_meter` both stay full-named because they coexist as alternatives.
- **Variant alias name:** `Any`. E.g. `Semantics::Any`, `Adapter::Any`, `notify::Any`. The sketch convention.
- **Worker `Model` nesting:** Yes. `user::Worker` holds behavior; `user::Worker::Model` holds the four message-type aliases plus capacity and period constants. Framework reads `User::Model::*` to wire the channel.
- **User classes location:** `tiny::user::Processor`, `tiny::user::Editor`, `tiny::user::Worker`. Distinguishes "things you write" from framework types.
- **Models location:** `tiny::models::Params`, `tiny::models::Meters` for the renamed existing models, plus **empty placeholder structs** for `tiny::models::Blocks`, `tiny::models::Tables`, `tiny::models::State` so the forthcoming feature PRs (per `plans/block-table-io.md` and `plans/state-model.md`) have reserved homes.
- **Migration strategy:** Big-bang single PR (within Phase 2) with a clean break — no `[[deprecated]]` aliases. All five demo plug-ins migrate in the same PR. The downstream `~/Developer/hii/` plug-in migrates in lock-step (separate commit, same window).
- **File reorganization:** Public headers (now under `include/tinyplug/` from Phase 1) get renamed to `tinyplug/<group>.h` (no `tiny_` prefix). Implementation headers move to `include/tinyplug/detail/`. The umbrella `tinyplug/tinyplug.h` stays as the public entry point.
- **Pending: singular vs plural namespaces.** `tiny::params` vs `tiny::param`, etc. Postponed by user; resolve before starting Phase 2 implementation.

### Phase 2 namespace tree

```
tiny::
├── params::      semantics, adapters, specs, registry — was tiny_params.h
├── meters::      ranges, specs, registry — was tiny_meters.h
├── events::      render + ui + action events, receiver struct — was tiny_events.h
├── view::        coords, pointer, contexts, notifications, time — was tiny_view.h
├── edit::        edit context, format, action queue, undo, state adapter — was tiny_edit.h + helpers
├── state::       persistence types, per-format rules — was state_adapter.hpp + state_rules.hpp
├── process::     transport, musical, dsp context, processor concept — was tiny_processor.h
├── worker::      actors, runner, runtime support for Worker — was tiny_worker.h
├── task::        manager, launcher, serial, notifications — was task_manager.hpp + friends
├── platform::    native view, dialogs, paths, window — was shared/platform/*.h
├── util::        Inline_visitor, Deferred, enum_raw, enumerate — was tiny_utils.h
├── user::        Processor, Editor, Worker (per-plug-in classes)
└── models::      Params, Meters, Blocks (planned), Tables (planned), State (planned)
```

### Phase 2 complete renaming table

### `tinyplug/params.h` (was `tiny_params.h`) — `namespace tiny::params`

| Old | New |
|---|---|
| `Bool_semantics` | `Semantics::Bool` |
| `List_semantics` | `Semantics::List` |
| `Int_semantics` | `Semantics::Int` |
| `Fixed_semantics` | `Semantics::Fixed` |
| `Real_semantics` | `Semantics::Real` |
| `Value_semantics` | `Semantics::Any` |
| `Adapt_lin` | `Adapter::Lin` |
| `Adapt_log` | `Adapter::Log` |
| `Adapt_pow` | `Adapter::Pow` |
| `Adapt_taper` | `Adapter::Taper` |
| `Adapt_piece` | `Adapter::Piece` |
| `Adapt_piece::Break_point` | `Adapter::Piece::Break_point` |
| `Knob_adapter` | `Adapter::Any` |
| `Host_policy` | `Policy` |
| `Units` | `Units` |
| `Param_order` | `Order` (values: `indexable`, `presentation`) |
| `Value_space` | `Space` (values: `plain`, `host`, `knob`) |
| `Param_spec` | `Spec` |
| `Param_group` | `Group` |
| `Param_node` | `Node` |
| `Value_conv` | `Conv` |
| `Host_formatter` | `Formatter` |
| `Some_param_model` (concept) | `Model` (concept) |
| `Param_infos<M>` | `Registry<M>` |
| `params_impl::` | `detail::` |
| free fns `get_plain_min/max/default`, `get_host_default`, `get_knob_default` | drop `get_` prefix: `plain_min`, `plain_max`, `plain_default`, `host_default`, `knob_default` |
| free fns `clamp`, `knob_next`, `param_is_discrete`, `is_param_units`, `units_string` | strip `param_`/`is_` redundancy: `clamp`, `knob_next`, `is_discrete`, `has_units`, `units_string` |
| `plain_to_norm`, `norm_to_plain` (overloads) | unchanged names, in `tiny::params::` |
| `make_array_by_indices` (template) | unchanged name |

### `tinyplug/meters.h` (was `tiny_meters.h`) — `namespace tiny::meters`

| Old | New |
|---|---|
| `Meter_policy` (peak/stream/trig) | `Policy` |
| `Lin_range` | `Range` |
| `Meter_spec` | `Spec` |
| `Tagged_meter` | `Tagged` |
| `Some_meter_model` (concept) | `Model` (concept) |
| `Meter_infos<M>` | `Registry<M>` |
| `plain_to_norm`, `norm_to_plain` overloads | unchanged |

### `tinyplug/events.h` (was `tiny_events.h`) — `namespace tiny::events`

| Old | New |
|---|---|
| `Set_param` | `Set_param` |
| `Ramp_param` | `Ramp_param` |
| `Accepted_latency` | `Accepted_latency` |
| `Render_event` | `Render` |
| `Tagged_event` | `Tagged` |
| `Set_meter` | `Set_meter` |
| `Ui_event` | `Ui` |
| `Action_start` | `Action_start` |
| `Action_end` | `Action_end` |
| `Request_resize` | `Request_resize` |
| `User_action` | `User_action` |
| `Ui_receiver` | `Ui_receiver` |

### `tinyplug/view.h` (was `tiny_view.h`) — `namespace tiny::view`

| Old | New |
|---|---|
| `Coords` | `Coords` |
| `Frame` | `Frame` |
| `Rect_size` | `Size` |
| `Modifier_keys` | `Modifiers` |
| `Pointer_button` | `Pointer::Button` |
| `Pointer_down/up/move/click/enter/exit/cancel` | `Pointer::Down/Up/Move/Click/Enter/Exit/Cancel` |
| `Pointer_event` | `Pointer::Event` |
| `Event` | `Event` |
| `Event_origin` | `Event_origin` |
| `Event_list` | `Event_list` |
| `Event_stream` | `Event_stream` |
| `User_interaction` | `Interaction` |
| `Scroll_data` | `Scroll` |
| `View_context` | `Context` |
| `Processor_view` | `Processor_view` |
| `Update_context` | `Update_context` |
| `Draw_context` | `Draw_context` |
| `Plugin_state` | `Plugin_state` |
| `Dark_mode_changed` | `notify::Dark_mode` |
| `Ui_notification` | `notify::Any` |
| `Draw_callback` | `Draw_callback` |
| `Notify_callback` | `Notify_callback` |
| `Steady_clock`, `Steady_time`, `System_clock`, `Time_point`, `Durations` | unchanged names in `tiny::view::` |
| `view_impl::run_frame` | `detail::run_frame` |

### `tinyplug/edit.h` (was `tiny_edit.h`) + impl headers under `detail/` — `namespace tiny::edit`

| Old | New |
|---|---|
| `Format` enum (Aax/Auv2/...) | `Format` (values lowercased: `aax, auv2, auv3, clap, vst3`) |
| `Edit_context` | `Context` |
| `Action_queue` | `Action_queue` |
| `Action_queue::Actor` | `Action_queue::Actor` |
| `Undo_history` | `Undo_history` |
| `Undo_history::Actor` / `::View` | unchanged nesting |
| `State_adapter` | `State_adapter` |
| `State_adapter::Actor` | unchanged nesting |
| `State_adapter::Provider/Load_model/Save_model` | unchanged nesting |

### `tinyplug/state.h` (new — combines state_adapter.hpp value types + state_rules.hpp) — `namespace tiny::state`

| Old | New |
|---|---|
| `State_tag` | `Tag` |
| `State_item` | `Item` |
| `State_map` | `Map` |
| `tag_for(item)` | `tag_for(item)` |
| `Maybe_values<T>` | `Maybe_values<T>` |
| `State_rules` | `Rules` |
| `State_rules::Aax/Auv2/Auv3/Clap/Vst3` | `Rules::Aax/Auv2/Auv3/Clap/Vst3` |

### `tinyplug/process.h` (was `tiny_processor.h`) — `namespace tiny::process`

| Old | New |
|---|---|
| `Transport_state` | `Transport` |
| `Time_sig` | `Time_sig` |
| `Musical_context` | `Musical` |
| `Dsp_context` | `Dsp` |
| `Some_plug_processor` (concept) | `Plugin` (concept) |
| `Processor_state` (currently in `tiny_utils.h`) | `State` (moves into `tinyplug/process.h`) |
| `frames_to_beats` | `frames_to_beats` |

### `tinyplug/worker.h` (was `tiny_worker.h`) — `namespace tiny::worker`

| Old | New |
|---|---|
| `Worker_actor<M>` | `Actor<M>` |
| `Worker_reply_actor<W>` | `Reply_actor<W>` |
| `Worker_runner<W>` | `Runner<W>` |
| `No_worker` | `None` |
| `User_worker` | `User` |
| `has_worker` (constexpr bool) | `present` (constexpr bool) |
| `Worker_processor_actor` | `Processor_actor` |
| `Worker_editor_actor` | `Editor_actor` |
| `Worker_replies` (alias) | `Replies` (alias for `Reply_actor<User>`) |
| `Receives_worker_reply_to_processor` (concept) | `Receives_to_processor` |
| `Receives_worker_reply_to_editor` (concept) | `Receives_to_editor` |
| `try_bind_worker` | `try_bind` |
| `try_drain_worker_to_processor` | `try_drain_to_processor` |
| `try_drain_worker_to_editor` | `try_drain_to_editor` |
| `TINY_HAS_WORKER` (macro) | unchanged (macros aren't namespaceable) |

### `tinyplug/task.h` (was `task_manager.hpp` + friends) — `namespace tiny::task`

| Old | New |
|---|---|
| `Task_manager` | `Manager` |
| `Task_manager::Actor` | `Manager::Actor` |
| `Task_launcher` | `Launcher` |
| `Task_launcher::Actor` | `Launcher::Actor` |
| `Serial_queue` | `Serial` |
| `Serial_queue::Actor` | `Serial::Actor` |
| `Notification_queue` | `Notifications` |
| `Task` (alias = `std::function<void()>`) | `Job` |

### `tinyplug/platform.h` (was `shared/platform/*.h`) — `namespace tiny::platform`

| Old | New |
|---|---|
| `Platform` (struct) | `Info` |
| `Platform::Type` | `Info::Type` |
| `Platform_view` | `View` |
| `Platform_views` | `Views` |
| `Platform_dialogs` | `Dialogs` |
| `Platform_binder` | `Binder` |
| `Platform_paths` | `Paths` |
| `Window_context` | `Window_context` |
| `View_delegate` | `View_delegate` |
| `Dark_mode_watcher` | `Dark_mode_watcher` |
| `Vsync_loop` | `Vsync_loop` |

### `tinyplug/util.h` (was `tiny_utils.h`) — `namespace tiny::util`

| Old | New |
|---|---|
| `Inline_visitor` | `Inline_visitor` |
| `Deferred` | `Deferred` |
| `Enum` (concept) | `Enum` |
| `enum_raw` | `enum_raw` |
| `enumerate` | `enumerate` |
| `deferred_false`, `_v` | `deferred_false`, `_v` |
| `is_variant_alternative` | `is_variant_alternative` |
| `normalized`, `denormalized` | `normalized`, `denormalized` |
| `Processor_state` | **moves** to `tiny::process::State` |

### Per-plug-in user code

| Old | New |
|---|---|
| `tiny::Plug_processor` (in `namespace tiny`) | `tiny::user::Processor` |
| `tiny::Plug_editor` | `tiny::user::Editor` |
| `tiny::Plug_worker` | `tiny::user::Worker` (with nested `Model` struct) |
| `tiny::Param_model` | `tiny::models::Params` |
| `tiny::Meter_model` | `tiny::models::Meters` |
| `Param_address` (nested enum in user's model) | `Address` (nested in `tiny::models::Params`) |
| `Meter_address` (nested enum) | `Address` (nested in `tiny::models::Meters`) |
| (new placeholder) | `tiny::models::Blocks { enum class Address { num_blocks }; };` |
| (new placeholder) | `tiny::models::Tables { enum class Address { num_tables }; };` |
| (new placeholder) | `tiny::models::State { enum class Address { num_states }; };` |

### `user::Worker::Model` shape

The four message types and tuning constants move from the worker class body into a nested `Model` struct:

| Today (on `Plug_worker`) | New (on `user::Worker::Model`) |
|---|---|
| `using From_processor = ...;` | `using From_processor = ...;` |
| `using From_editor = ...;` | `using From_editor = ...;` |
| `using To_processor = ...;` | `using To_processor = ...;` |
| `using To_editor = ...;` | `using To_editor = ...;` |
| `static constexpr auto inbound_capacity` | `static constexpr auto inbound_capacity` |
| `static constexpr auto reply_capacity` | `static constexpr auto outbound_capacity` (renamed for symmetry, per sketch) |
| `static constexpr auto poll_interval` | `static constexpr auto update_period` (per sketch) |

The framework's `worker::Runner`, queue typedefs, and concepts query `User::Model::*` instead of `User::*`.

### Phase 2 header file reorganization

```
include/tinyplug/
├── tinyplug.h               (umbrella, unchanged role; includes all the below)
├── params.h                 (was tiny_params.h)
├── meters.h                 (was tiny_meters.h)
├── events.h                 (was tiny_events.h)
├── view.h                   (was tiny_view.h)
├── edit.h                   (public surface of tiny_edit.h + action/undo/state_adapter)
├── state.h                  (state items + state_rules)
├── process.h                (was tiny_processor.h)
├── worker.h                 (was tiny_worker.h)
├── task.h                   (re-exports task_manager + launcher + serial + notifications)
├── platform.h               (re-exports shared/platform/*.h types)
├── util.h                   (was tiny_utils.h)
└── detail/
    ├── action_queue.hpp
    ├── undo_history.hpp
    ├── state_adapter.hpp
    ├── state_rules.hpp
    ├── lock_free_queue.hpp
    ├── gesture_recognizers.hpp
    ├── change_list.hpp
    ├── task_manager.hpp
    ├── task_launcher.hpp
    ├── serial_queue.hpp
    └── notification_queue.hpp
```

`tinyplug.h` orders includes so `worker.h` (with its `__has_include("plug_worker.h")` discovery) runs last, the same way it does today.

Plug-in code only references `#include "tinyplug/tinyplug.h"` — same as today — so no per-plug-in include changes needed beyond the namespace-qualification updates.

### Phase 2 files to modify

#### Framework headers (all in `include/tinyplug/`):
Rename + edit:
- [tiny_params.h](include/tinyplug/tiny_params.h) → `params.h`
- [tiny_meters.h](include/tinyplug/tiny_meters.h) → `meters.h`
- [tiny_events.h](include/tinyplug/tiny_events.h) → `events.h`
- [tiny_view.h](include/tinyplug/tiny_view.h) → `view.h`
- [tiny_edit.h](include/tinyplug/tiny_edit.h) → `edit.h`
- [tiny_processor.h](include/tinyplug/tiny_processor.h) → `process.h`
- [tiny_worker.h](include/tinyplug/tiny_worker.h) → `worker.h`
- [tiny_utils.h](include/tinyplug/tiny_utils.h) → `util.h`
- [tinyplug.h](include/tinyplug/tinyplug.h) — update includes

Move to `detail/`:
- [action_queue.hpp](include/tinyplug/action_queue.hpp), [action_queue.cpp](include/tinyplug/action_queue.cpp)
- [undo_history.hpp](include/tinyplug/undo_history.hpp), [undo_history.cpp](include/tinyplug/undo_history.cpp)
- [state_adapter.hpp](include/tinyplug/state_adapter.hpp), [state_adapter.cpp](include/tinyplug/state_adapter.cpp)
- [state_rules.hpp](include/tinyplug/state_rules.hpp)
- [lock_free_queue.hpp](include/tinyplug/lock_free_queue.hpp)
- [gesture_recognizers.hpp](include/tinyplug/gesture_recognizers.hpp), [gesture_recognizers.cpp](include/tinyplug/gesture_recognizers.cpp)
- [change_list.hpp](include/tinyplug/change_list.hpp)
- [task_manager.hpp](include/tinyplug/task_manager.hpp), [task_launcher.hpp](include/tinyplug/task_launcher.hpp), [serial_queue.hpp](include/tinyplug/serial_queue.hpp), [notification_queue.hpp](include/tinyplug/notification_queue.hpp) (+ `.cpp` siblings)

Create new public re-export headers:
- `include/tinyplug/state.h` (combines state_adapter value types + state_rules)
- `include/tinyplug/task.h` (re-exports task_manager + launcher + serial + notifications)
- `include/tinyplug/platform.h` (re-exports from `shared/platform/`)

Update:
- [shared/CMakeLists.txt](shared/CMakeLists.txt) — header lists for IDE; detail moved files.

#### Format wrappers (all under `wrappers/` after Phase 1):

Every wrapper file references framework types. Bulk find-and-replace per the renaming tables.

- AAX: [aax_parameters.{h,cpp}](formats/aax/source/aax_parameters.h), [aax_gui.{h,cpp}](formats/aax/source/aax_gui.h), [aax_adapters.h](formats/aax/source/aax_adapters.h), [aax_monolith.cpp](formats/aax/source/aax_monolith.cpp), [aax_describe.cpp](formats/aax/source/aax_describe.cpp)
- AUv2: [auv2_effect.{h,cpp}](formats/auv2/source/auv2_effect.h), [auv2_view.{h,cpp}](formats/auv2/source/auv2_view.h), [auv2_adapters.h](formats/auv2/source/auv2_adapters.h), `auv2_view_factory.mm`
- AUv3: [DSPKernel.hpp](formats/auv3/source/extension/DSPKernel.hpp), [auv3_AUAudioUnit.{h,mm}](formats/auv3/source/extension/auv3_AUAudioUnit.h), [auv3_AUViewController.mm](formats/auv3/source/extension/auv3_AUViewController.mm), [auv3_view.{h,cpp}](formats/auv3/source/extension/auv3_view.h)
- CLAP: [clap_plugin.{h,cpp}](formats/clap/source/clap_plugin.h), [clap_view.{h,cpp}](formats/clap/source/clap_view.h), [clap_adapters.h](formats/clap/source/clap_adapters.h), [clap_entry.cpp](formats/clap/source/clap_entry.cpp)
- VST3: [vst3_processor.{h,cpp}](formats/vst3/source/vst3_processor.h), [vst3_controller.{h,cpp}](formats/vst3/source/vst3_controller.h), [vst3_view.{h,cpp}](formats/vst3/source/vst3_view.h), [vst3_messaging.{h,cpp}](formats/vst3/source/vst3_messaging.h), [vst3_adapters.h](formats/vst3/source/vst3_adapters.h), [vst3_entry.cpp](formats/vst3/source/vst3_entry.cpp)

#### Demo plug-ins (per plug-in `source/` under `examples/`):

For each of `gain_demo`, `automation_tester`, `latency_demo`, `platform_demo`, `worker_demo`:
- `models/param_model.h` → `models/params.h` (or keep filename, just rewrite contents)
- `models/meter_model.h` → `models/meters.h`
- `plug_processor.{h,cpp}` — change `class Plug_processor` to `namespace tiny::user { class Processor`, update all framework refs.
- `plug_editor.{h,cpp}` — same shape.
- For `worker_demo`: `plug_worker.h` — `class Plug_worker` → `namespace tiny::user { class Worker`, move members into nested `struct Model`, rename `reply_capacity` → `outbound_capacity`, `poll_interval` → `update_period`.

CMakeLists.txt files per plug-in stay as-is (source paths unchanged unless we rename `param_model.h` → `params.h`).

#### Template:
- [template/source/](template/source/) gets the same migration as the demos.
- [new_plugin.py](new_plugin.py) — no logic changes; the template scaffold is what migrates.

#### Downstream:
- `~/Developer/hii/` plug-in needs the same migration. Separate commit, same window.

### Phase 2 execution order (within the single Phase 2 PR)

To minimize "everything broken at once":

1. **Move/rename framework headers and create new umbrella include layout** (no body edits yet). Each renamed header still works because `tinyplug.h` is updated to include the new paths. Validate: framework alone compiles (`tiny_shared_lib`).
2. **Add new namespaces around existing content** — wrap each header's body in `namespace tiny::<group> { ... }` but leave type names unchanged. At the bottom of each header, add `using` aliases at the old `tiny::` location so wrapper code still compiles. Validate: full build green.
3. **Rename types inside each namespace**, one group at a time (`params`, then `meters`, then `events`, etc.). Update aliases. Validate: full build green between each group.
4. **Remove the `tiny::` `using` aliases** and update all wrapper / demo references to the new qualified names. This is the bulk find-and-replace step. Validate: full build green.
5. **Move `Plug_processor`/`Plug_editor`/`Plug_worker` into `tiny::user::`** and `Param_model`/`Meter_model` into `tiny::models::`. Update wrapper discovery and demo plug-ins. Add empty `Blocks`, `Tables`, `State` placeholders in `tiny::models::`. Validate: full build green.
6. **Restructure `user::Worker`** — move messages and constants into nested `Model` struct; rename `reply_capacity` → `outbound_capacity`, `poll_interval` → `update_period`. Update framework worker runner and concepts. Update `worker_demo::plug_worker.h`. Validate: WorkerDemo VST3 builds and the demo plug-in's transport-events round-trip works in a host.
7. **Move impl headers to `detail/`** and update `#include` paths in `.cpp` files. Validate: full build green.
8. **Smoke test in a host** — load worker_demo VST3, confirm transport events still flow to the JSON log.

### Phase 2 verification

End-to-end checks, in order:

1. **`tiny_shared_lib` compiles** after step 1 (header rename only).
2. **Full build green** at the end of each step 2–7. Always run `cmake --build build` serially (no `--parallel`).
3. **All 30 build targets pass** after step 4 and again after step 7.
4. **Symbol-count sanity** — confirm worker symbols still absent in gain_demo VST3 and present in worker_demo VST3 (same check as the worker opt-in gating).
5. **Host smoke test:** load WorkerDemo VST3 in Bitwig and Live; trigger Set_session from editor, hit play, confirm transport events land in the session log file as before the refactor.
6. **Wrapper sanity:** load AutomationTester (no worker) in a host; confirm params automate and gestures still register, no regressions.
7. **Downstream:** update `~/Developer/hii/` plug-in in lock-step; confirm it builds and the timeline logger still works.

## Open items (cross-phase)

- **`Notifications` vs `Notification` for `task::Notifications`** (the queue type). Plural reads slightly off but disambiguates from `view::notify::Any` (UI notifications). Defer; defaulting to `Notifications` in the table above.
- **Whether to also rename `Tagged_event` and `Tagged_meter` to `events::Tagged` and `meters::Tagged`** — both are namespaced, both lose the disambiguating context-word. They might be better as `events::Tagged_event` and `meters::Tagged_meter` for clarity at use sites. Decide during step 3.
- **`view::Pointer::Button` enum values** (`left`, `right`) — currently `enum class Pointer_button : uint32_t { left, right }`. Nesting under `Pointer::` makes the enum name redundant; consider just `Button` (already proposed) and lowercase values stay.
- **`change_list.hpp` public-vs-detail decision.** Used by the downstream `hii` plug-in. If it's an external API surface, promote to `tinyplug/change_list.h` (public). If only the framework uses it, keep in `detail/`. Verify against `hii` usage during step 7.

