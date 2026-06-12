# Migration guide (refactor on `next`)

This is a **living document** tracking source-breaking changes on the `next`
branch so downstream plug-ins can migrate. It is appended to as the refactor
continues — sections are roughly in the order the changes landed.

> Scope note: most changes are mechanical (renames). Where a change is partial
> (some params helpers are still transitional aliases), it's called out so you
> don't over-migrate.

## Status at a glance

| Area | State |
|---|---|
| All headers `.h` → `.hpp` | ✅ done |
| User files/classes renamed + namespaced (`models::`, `plugin::`) | ✅ done |
| Address enums → nested `Address`, **capitalized cases + `Num_*` sentinel** | ✅ done |
| Meters: `make_spec` API, `tiny::meters`, capitalized `Policy` | ✅ done |
| **Params → `tiny::params`**: `Semantics`, `Adapter`, `Spec`/`Group`/`Node`, `Policy`, `Units` caps, `Infos`/`Model` | ✅ done |
| Enum-case capitalization convention | ✅ in progress (params/meters done) |
| `Spec`/`Group`/`List` strings → `std::string_view` | ✅ done |
| Format wrapper files/classes → per-format namespaces | ✅ done (internal) |
| **Value helpers → `params::Value_helper`** (was `Value_conv` + free fns), `Value_space`→`Space`, own `value_helper.{hpp,cpp}` | ✅ done |
| **Platform → its own `tiny_platform` lib**; `PLATFORM_*`→`TINY_PLATFORM_*`, `Platform` struct removed; Skia PRIVATE | ✅ done (iOS/Win pending verify) |
| **Directory restructure → `libs/` layout** + `tiny_dsp` lib; includes `<tiny_platform/…>` / `<tiny_dsp/…>`; `tiny_platform.hpp`→`platform_defs.hpp` | ✅ done (Win pending verify) |
| Params leftovers: `Host_formatter`→`Formatter`, `Param_order`→`Order`, `Units` | ⏳ still transitional `tiny::` aliases |
| Worker nested `Model` restructure | ⏳ not yet migrated |
| `tiny::events` / `tiny::view` / … namespace passes | ⏳ not yet migrated |

---

## 1. Header extension: `.h` → `.hpp`

All C++ headers are now `.hpp`. Objective-C headers (`@interface`) stay `.h`.

- Rename your headers `*.h` → `*.hpp` and update `#include`s.
- **Worker discovery:** the framework now does `__has_include("worker.hpp")`
  (was `plug_worker.h`). See §2 — your worker file must be `worker.hpp` or the
  worker silently disconnects.

## 2. User files renamed

| Old | New |
|---|---|
| `source/models/param_model.h` | `source/models/params.hpp` |
| `source/models/meter_model.h` | `source/models/meters.hpp` |
| `source/plug_processor.{h,cpp}` | `source/processor.{hpp,cpp}` |
| `source/plug_editor.{h,cpp}` | `source/editor.{hpp,cpp}` |
| `source/plug_worker.h` | `source/worker.hpp` |

Update the `target_sources(...)` list in your plug-in's `CMakeLists.txt` to match.

## 3. User classes moved into namespaces

| Old (`namespace tiny`) | New |
|---|---|
| `tiny::Param_model` | `tiny::models::Params` |
| `tiny::Meter_model` | `tiny::models::Meters` |
| `tiny::Plug_processor` | `tiny::plugin::Processor` |
| `tiny::Plug_editor` | `tiny::plugin::Editor` |
| `tiny::Plug_worker` | `tiny::plugin::Worker` |

Wrap each definition file's body in the new namespace and drop the prefix
(`namespace tiny::plugin { class Processor … }`). Framework types (`Set_param`,
`Plugin_state`, `Edit_context`, `enum_raw`, …) remain in `tiny::` and resolve
unqualified from inside `tiny::plugin` / `tiny::models`.

## 4. Address enums → nested `Address` (+ capitalized cases)

The per-model address enum is now named `Address` (was `Param_address` /
`Meter_address`), nested in the model, with **capitalized cases** and a
**capitalized sentinel** that the concepts now require:

```cpp
// models/params.hpp
namespace tiny::models {
struct Params {
    enum class Address : uint32_t { Gain = 0, Num_params };   // was { gain, num_params }
};
}
// models/meters.hpp → enum class Address { …, Num_meters };
```

In `processor.hpp`/`editor.hpp`, the convenience alias + usages:

```cpp
using Address = models::Params::Address;
// usage: enum_raw(Address::Gain)
```

**Collision caveat:** a file that needs *both* the param and meter address enums
can't alias both to `Address`. Keep `Address` for params and qualify the meter
one inline, e.g. `models::Meters::Address::My_meter`.

## 5. Meters API + `tiny::meters` namespace

The meter model returns **one spec per address** via a switch (no vector);
`Spec` has no `address` field; the sentinel must be `Num_meters`.

```cpp
namespace tiny::models {
struct Meters {
    enum class Address : uint32_t { My_meter = 0, Num_meters };
    static auto make_spec(Address address) -> meters::Spec {
        using namespace meters;
        switch (address) {
            case Address::My_meter: return { .range = Range{0, 1}, .policy = Policy::Stream };
            default:                return {};   // required (-Wswitch-default)
        }
    }
};
static_assert(meters::Model<Meters>);
}
// zero-meter model: static auto make_spec(Address) -> meters::Spec { return {}; }
```

Meter type renames:

| Old (`tiny::`) | New |
|---|---|
| `Meter_policy` | `meters::Policy` (members `Peak`/`Stream`/`Trig`) |
| `Lin_range` | `meters::Range` |
| `Meter_spec` | `meters::Spec` (just `{ range, policy }`) |
| `Some_meter_model` | `meters::Model` (concept) |
| `Meter_infos<M>` | `meters::Infos<M>` |
| `Tagged_meter` | `tiny::view::Meter_state` (view-layer; internal) |

`meters::plain_to_norm`/`norm_to_plain` are now VST3-internal.

## 6. Params → `tiny::params` namespace

`tiny_params.hpp` is now `namespace tiny::params`. Type renames:

| Old (`tiny::`) | New |
|---|---|
| `Bool_semantics` / `List_semantics` / `Int_semantics` / `Fixed_semantics` / `Real_semantics` | `params::Semantics::{Bool, List, Int, Fixed, Real}` |
| `Value_semantics` | `params::Semantics::Any` |
| `Adapt_lin` / `Adapt_log` / `Adapt_pow` / `Adapt_taper` / `Adapt_piece` | `params::Adapter::{Lin, Log, Pow, Taper, Piece}` |
| `Adapt_piece::Break_point` | `params::Adapter::Piece::Break_point` |
| `Knob_adapter` | `params::Adapter::Any` |
| `Param_spec` / `Param_group` / `Param_node` | `params::Spec` / `params::Group` / `params::Node` |
| `Host_policy` | `params::Policy` (members `Automation`/`Control`/`Hidden`/`Interface`) |
| `Units::generic` … | `params::Units::Generic` … (type still `Units`, members capitalized) |
| `Param_infos<M>` | `params::Infos<M>` |
| `Some_param_model` (concept) | `params::Model` |

A migrated param model:

```cpp
// models/params.hpp
namespace tiny::models {
struct Params {
    enum class Address : uint32_t { Gain = 0, Num_params };

    static auto build_tree() -> params::Node
    {
        using enum Address;
        return params::Group{ .nodes = {
            params::Spec{
                .address = enum_raw(Gain),
                .string_id = "gain",
                .name = "Gain",
                .semantics = params::Semantics::Real{
                    .min_val = 0, .def_val = 1, .max_val = 1,
                    .units = params::Units::Generic,
                    .knob_adapter = params::Adapter::Pow{3},
                },
                .policy = params::Policy::Automation,
            }
        }};
    }
};
static_assert(params::Model<Params>);
}
```

In `processor.hpp`/`editor.hpp`: `using User_params = params::Infos<models::Params>;`.

**Still transitional** (kept as `using` aliases in `tiny::`, so old spellings
still compile for now — will migrate later): `Units`, `Host_formatter`,
`Param_order`. You may qualify them with `params::` now if you like; not
required yet. (The value-conversion helpers were consolidated — see §6a.)

## 6a. Value helpers → `params::Value_helper` (new `value_helper.{hpp,cpp}`)

The conversion/query free functions and the old `Value_conv` struct were
consolidated into a single `params::Value_helper` struct and moved out of
`tiny_params.hpp` into **`value_helper.hpp` / `value_helper.cpp`**, so the params
header stays declarative. `tinyplug.hpp` includes the new header, so anything
that already includes the umbrella sees it automatically.

| Old | New |
|---|---|
| `Value_conv::plain_to_host` (etc., all 6 directions) | `Value_helper::plain_to_host` (etc., unchanged names) |
| `plain_to_norm` / `norm_to_plain` | `Value_helper::plain_to_knob` / `Value_helper::knob_to_plain` (knob == norm) |
| `get_plain_default` / `get_host_default` / `get_knob_default` | `Value_helper::default_value(spec, Space::{Plain,Host,Knob})` |
| `get_plain_min` / `get_plain_max` | `Value_helper::plain_min` / `Value_helper::plain_max` (or just `Value_helper::clamp`) |
| `param_is_discrete` / `is_param_units` | `Value_helper::is_discrete` / `Value_helper::has_units` |
| `units_string` | `Value_helper::units_label` |
| `clamp` / `knob_next` (templates) | `Value_helper::clamp` / `Value_helper::knob_next` (now plain `double`) |
| `Value_space` | `Space` |
| `Model::make_defaults<T>(space)` (Infos member) | `make_defaults<T, Infos>(space)` (free template) |
| `make_array_by_indices` | unchanged name, now in `value_helper.hpp` |

New helpers filling out the set: `Value_helper::convert(v, from, to, semantics)`
and `Value_helper::quantize(v, semantics)` (the `norm_to_plain(plain_to_norm(…))`
step-snap idiom). `default_value` for `Fixed` host space now quantizes (was raw);
no effect for step-aligned defaults, which `validate_spec` already requires.

Transitional `tiny::` aliases exist for `Value_helper`, `Space`, `make_defaults`,
`make_array_by_indices`, so unqualified `tiny::` spellings keep compiling.

## 7. Enum-case capitalization convention

Scoped-enum **cases are now Capitalized** (new house style). Done for params and
meters: `Units::Generic`, `Policy::Automation`, `meters::Policy::Stream`, and the
model address cases + `Num_params` / `Num_meters` sentinels. When you migrate your
models, capitalize your address cases and update the matching `enum_raw(Address::…)`
usages. (Note: `using enum` scopes reference cases bare — `case Automation:` etc.)

## 8. `Spec` / `Group` / `List` strings → `std::string_view`

`Group`/`Spec` `name`, `string_id`, `short_name` and `Semantics::List::items` are
now `std::string_view` (`items` is `std::vector<std::string_view>`).

- **Authoring is unchanged** — string literals (`.name = "Gain"`,
  `items{"One","Two"}`) bind to `string_view` exactly as before; same bytes, same
  runtime behavior. Lifetime is the same (non-owning; literals are static).
- Only caveat: don't assign a temporary `std::string` to these fields (dangles —
  same footgun as the old `const char*`). Keep using literals / static storage.
- `Spec`/`Group` `operator==` is now content-based (was pointer-based); unused in
  the framework, so no behavioral impact.

## 9. Format wrapper internals (only if you forked a wrapper)

Plug-in authors normally don't touch these. Files dropped their format prefix
(`vst3_controller.hpp` → `controller.hpp`, `vst3_processor` → `audio_effect`,
`auv3_AUAudioUnit` → `audio_unit` *file*, class name kept); classes moved into
per-format namespaces (`tiny::vst3::Audio_effect/Controller/View`,
`tiny::clap::Plugin/View`, `tiny::auv2::Effect/View`, `tiny::auv3::View`,
`tiny::aax::Parameters/Gui`); generated headers dropped prefixes
(`aax_categories`→`categories`, `*_preset_list`→`preset_list`).

**AUv2 entry point changed:** the component factory is now `EffectFactory`
(was `Auv2_effectFactory`). If you maintain a custom AUv2 `Info.plist` /
`exports.txt`, update `factoryFunction` → `EffectFactory` and export
`_EffectFactory`. (Identification is still by AudioComponent codes, so installed
plug-ins / saved projects are unaffected.)

## 10. CMake

- Update your plug-in `CMakeLists.txt` `target_sources` to the new file names (§2).
- `configure_plug_info` / generated `plug_info.hpp` (note `.hpp`) unchanged in role.

## 11. Platform library split (new — affects your plug-in `CMakeLists.txt`)

The platform layer (native view, dialogs, paths, window/Skia surface) is now a
separate static library, **`tiny_platform`**, sitting on top of the core
**`tiny_shared_lib`**. Dependency is strictly one-way (`tiny_platform → tiny_shared_lib`).

**Plug-in link line changed.** Where you previously had:

```cmake
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_SHARED_LIB})
```

use:

```cmake
target_link_libraries(${PLUGIN_TARGET} PUBLIC  ${TINY_SHARED_LIB})   # core (your public headers expose core types)
target_link_libraries(${PLUGIN_TARGET} PRIVATE ${TINY_PLATFORM_LIB}) # editor may use dialogs/paths
target_link_libraries(${PLUGIN_TARGET} PRIVATE tiny::skia)           # editor draws with Skia
```

`${TINY_PLATFORM_LIB}` (= `tiny_platform`) is forwarded to parent scope alongside
`${TINY_SHARED_LIB}`. Skia is now **PRIVATE** (an implementation detail) — link it
explicitly wherever you compile against it (any editor that draws). The per-format
`make_<format>_plugin` wrappers link `tiny_platform` themselves; you don't.

**Platform macros renamed + `Platform` struct removed.** If your code does
compile-time OS selection:

| Old | New |
|---|---|
| `PLATFORM_MACOS` / `PLATFORM_IOS` / `PLATFORM_APPLE` / `PLATFORM_WINDOWS` | `TINY_PLATFORM_MACOS` / `…IOS` / `…APPLE` / `…WINDOWS` |
| `Platform::resolved == Platform::Type::macos` (struct, **removed**) | `#if TINY_PLATFORM_MACOS` … `#endif` |

The macros now live in core at `<tinyplug/platform_defs.hpp>` (OS detection is a core
concern; **renamed** from `tinyplug/tiny_platform.hpp` in §12). Include that header where
you use the macros — don't rely on getting them transitively. The platform umbrella
`<tiny_platform/tiny_platform.hpp>` re-exports the platform surface (view, dialogs,
paths, defs) for wrapper-level code.

> The include spellings above reflect the §12 directory restructure. If you're
> migrating in order, the platform/dsp headers became `<tiny_platform/...>` /
> `<tiny_dsp/...>` angle includes at that step — see §12.

## 12. Directory restructure → `libs/` layout (affects your `#include`s + CMake)

The framework, platform, and DSP layers are now three peer libraries under `libs/`,
each with its own **isolated** include root. A consumer only sees a library's headers
if it links that library — so the include spellings for the platform and DSP headers
changed to angle-bracket form, and DSP now needs an explicit link.

```
libs/tinyplug/       → <tinyplug/...>        (core; umbrella <tinyplug/tinyplug.hpp>)
libs/tiny_platform/  → <tiny_platform/...>   (umbrella <tiny_platform/tiny_platform.hpp>)
libs/tiny_dsp/       → <tiny_dsp/...>        (header-only)
```

**The core umbrella include is unchanged** — `#include <tinyplug/tinyplug.hpp>` still
works (most plug-ins only need this). The CMake target names are unchanged
(`${TINY_SHARED_LIB}`, `${TINY_PLATFORM_LIB}`), plus a new `${TINY_DSP_LIB}`.

### Include changes

| Old | New |
|---|---|
| `#include "tinyplug/tiny_platform.hpp"` | `#include <tinyplug/platform_defs.hpp>` (OS macros; **renamed**) |
| `#include "platform/platform.hpp"` | `#include <tiny_platform/tiny_platform.hpp>` |
| `#include "platform/platform_view.hpp"` | `#include <tiny_platform/platform_view.hpp>` |
| `#include "platform/platform_dialogs.hpp"` | `#include <tiny_platform/platform_dialogs.hpp>` |
| `#include "platform/platform_paths.hpp"` | `#include <tiny_platform/platform_paths.hpp>` |
| `#include "platform/window_context.hpp"` | `#include <tiny_platform/window_context.hpp>` |
| `#include "dsp/host_bypass.hpp"` | `#include <tiny_dsp/host_bypass.hpp>` |
| `#include "dsp/linear_ramper.hpp"` | `#include <tiny_dsp/linear_ramper.hpp>` |
| `#include "dsp/delay_line.hpp"` | `#include <tiny_dsp/delay_line.hpp>` |

General rule: `"platform/X.hpp"` → `<tiny_platform/X.hpp>`, `"dsp/X.hpp"` →
`<tiny_dsp/X.hpp>`. (Your own plug-in-local `source/dsp/...` helpers are unaffected —
those are your files, not the framework's.)

### CMake: link `tiny_dsp` if you use the DSP helpers

Header isolation means `<tiny_dsp/...>` won't resolve unless you link the lib. If your
processor/editor uses `Host_bypass`, `Linear_ramper`, or `Delay_line`:

```cmake
target_link_libraries(${PLUGIN_TARGET} PRIVATE ${TINY_DSP_LIB})   # header-only, zero binary cost
```

`${TINY_DSP_LIB}` (= `tiny_dsp`) is forwarded to parent scope alongside the other two.
The per-format `make_<format>_plugin` wrappers already link `tiny_dsp` and
`tiny_platform` themselves; this line is only needed if *your* code (not the wrapper)
includes a DSP header. The §11 platform/Skia link lines are unchanged.

> Repo-internal note (not a source change): `formats/` → `wrappers/`,
> `plugins/` → `examples/`. Only relevant if you reference tinyplug's tree by path.

---

## Not yet migrated (don't change these yet)

- **Params leftovers:** `Host_formatter` (→ `Formatter`), `Param_order`
  (→ `Order`), and `Units` (type kept) are still reachable via transitional
  `tiny::` aliases and move into `tiny::params` in later steps. (The value
  helpers and `Value_space`→`Space` are done — see §6a.)
- **Worker internals:** `worker.hpp` / `tiny::plugin::Worker` is the only worker
  change so far. The message-type aliases and tuning constants
  (`From_processor`, `reply_capacity`, `poll_interval`, …) keep their current
  shape — the nested `Model` restructure hasn't landed.
- **Other framework sub-namespaces** (`tiny::events`, `tiny::view`, `tiny::edit`,
  `tiny::state`, `tiny::process`, `tiny::task`, `tiny::worker`, `tiny::util`).
