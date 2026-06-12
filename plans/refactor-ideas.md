# Refactor ideas (future backlog)

The big structural + naming refactor has **landed** — `libs/` layout with isolated
include roots, the `params`/`meters`/`models`/`plugin` namespaces, `CMakePresets`,
the worker `Model` restructure, the rebuilt template + generator, and the `tools/`
consolidation. This file is just what's *left*: optional, non-blocking ideas for when
there's appetite. Nothing here is committed work.

> **The bar for the naming items:** pursue a sub-namespace only if it's a genuine
> *organizational concept* (the bar set by `params`/`meters`), not for tidiness.

## Build infrastructure

The remaining build wins. **CI is the standout** — it makes the multi-platform /
multi-format matrix self-verifying instead of hand-checked (Windows/iOS/AUv3 are the
paths that are painful to verify locally).

| Idea | Effort | Impact |
|---|---|---|
| **CI** — `.github/workflows/build.yml` (macOS: Makefiles for the 4 formats + Xcode for AUv3; a Windows job) + `lint.yml` (`clang-format --dry-run --Werror`). Trigger on push to `main`/`next` + every PR. | — | Auto-catches cross-platform/format regressions; the canary for any structural change. |
| **`.clang-format`** matching the README's Stroustrup-lean style | ~1h | Consistency; enables IDE auto-format |
| **`.editorconfig`** | ~30m | Tabs/spaces/EOL agreement across editors |
| **PCH** per wrapper for the heavy SDK header (`<public.sdk/...>`, `<AAX_*.h>`, `<AudioUnitSDK/...>`) | 4–6h | ~20–30% wrapper compile reduction |
| **Unity builds** per wrapper (`UNITY_BUILD ON`) | 2–3h | ~15–25% wall-clock per wrapper |

(`CMakePresets.json` already covers the no-`-D`-flags onboarding UX.)

## Namespace polish (low priority)

The core grouping landed. Further sub-namespaces are **opt-in** — only worth it where a
group is a real concept. Mechanic (the proven `params`/`meters` playbook): wrap the
header in `namespace tiny::<group>`, add transitional `using` aliases at the old `tiny::`
location, migrate refs, drop the aliases, build between each.

- **Small finish-ups** — the last un-namespaced param types: `Host_formatter` →
  `params::Formatter`, `Param_order` → `params::Order`. (`Units` stays; `Value_helper` /
  `Infos` names are final.)
- **Candidate groups, if ever pursued** (headline renames only — design fresh, don't treat as spec):

  | Group | Holds | Headline renames |
  |---|---|---|
  | `events::` | render / ui / action events | `Render_event`→`Render`, `Ui_event`→`Ui` (keep `Set_param`/`Set_meter` full) |
  | `view::` | coords, pointer, contexts | `Rect_size`→`Size`, `Pointer_*`→`Pointer::*`, `Ui_notification`→`notify::Any` |
  | `process::` | transport, dsp ctx, concept | `Dsp_context`→`Dsp`, `Transport_state`→`Transport`, `Some_plug_processor`→`Plugin` |
  | `task::` | manager / launcher / serial / queues | `Task_manager`→`Manager`, `Serial_queue`→`Serial`, `Task`→`Job` |
  | `worker::` | actors / runner | `Worker_actor`→`Actor`, `Worker_runner`→`Runner`, `No_worker`→`None` |
  | `edit::` / `state::` | edit ctx, persistence | `Edit_context`→`Context`, `State_rules`→`Rules`, `State_map`→`Map` |
  | `util::` | visitor / enum helpers | mostly unchanged; `Processor_state` would move to `process::State` |
  | `platform::` | the `tiny_platform` types | `Platform_view`→`View`, `Platform_dialogs`→`Dialogs` |

- **Open naming nits** (decide if/when the owning group is touched):
  - `task::Notifications` (plural, to disambiguate from `view::notify::Any`) vs `Notification`.
  - `Tagged_event` / `Tagged_meter` → `events::Tagged` / `meters::Tagged`, or keep the context word for use-site clarity.
  - `view::Pointer::Button` — the enum name is redundant once nested under `Pointer::`.
- **Pending question:** singular vs plural namespaces (`params` vs `param`). Resolve before any pass.

## Header organization

- **`detail/` split** — move impl-only headers (`action_queue`, `undo_history`,
  `state_adapter`, `lock_free_queue`, `gesture_recognizers`, `task_*`, `serial_queue`,
  `notification_queue`) under `libs/tinyplug/include/tinyplug/detail/`, leaving the public
  surface clean. Natural to pair with a namespace pass.
- **`change_list.hpp` public-vs-detail** — used by downstream `hii`. If it's external API,
  keep it public (`tinyplug/`); if framework-only, move to `detail/`. Verify against `hii`.

## Model placeholders for upcoming features

Add empty `tiny::models::{Blocks, Tables, State}` structs (each
`enum class Address { Num_* };`) to reserve named homes for the
[block-table-io.md](block-table-io.md) / [state-model.md](state-model.md) feature PRs,
so those land without churning the demos.

## Optional-lib architecture (as `libs/` grows)

`libs/` will gain `tiny_ui`, `tiny_text`, … . One invariant keeps it clean:

> **The dependency graph is a DAG; every edge points _down_ toward core (or a lower
> lib). Core never depends on an optional lib; no cycles.**

- A type enters **core** only if it's *foundational AND used by ≥2 consumers*; lib-specific
  types stay in the lib. `tiny_dsp` should keep its zero-core-dep (pure leaf) status.
- **Escape valve (only if core grows heavy):** extract a thin `tiny_base` (Task_manager,
  lock-free queue, util, view primitives) *below* core and have libs depend on `base`.
  Don't do it preemptively — it adds a layer.

## Loose ends

- **Downstream `~/Developer/hii`** — migrate to the new include paths / link lines (MIGRATION §11–13).
- **`Value_helper` unit test** — the one logic-dense untested piece (plus the
  `default_value(Fixed, Host)` quantize tweak that landed during the refactor).
- **AGENTS.md deep per-format path links** — still stale from the filename de-prefixing; a mechanical sweep.
