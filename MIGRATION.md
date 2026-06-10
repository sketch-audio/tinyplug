# Migration guide (refactor on `next`)

This is a **living document** tracking source-breaking changes on the `next`
branch so downstream plug-ins can migrate. It is appended to as the refactor
continues — sections are roughly in the order the changes landed.

> Scope note: most changes are mechanical (renames). Where a change is partial
> (e.g. params got renamed but not yet fully namespaced), it's called out so you
> don't over-migrate.

## Status at a glance

| Area | State |
|---|---|
| All headers `.h` → `.hpp` | ✅ done |
| User files/classes renamed + namespaced (`models::`, `plugin::`) | ✅ done |
| Address enums → nested `Address` | ✅ done |
| Meters: `make_spec` API, `tiny::meters` namespace, capitalized `Policy` | ✅ done |
| Format wrapper files/classes → per-format namespaces | ✅ done (internal) |
| **Params**: still `build_tree()`, flat `tiny::` types, lowercase `num_params` | ⏳ not yet migrated |
| Worker nested `Model` restructure | ⏳ not yet migrated |
| `tiny::params` / `tiny::events` / … namespace pass | ⏳ not yet migrated |

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

In each definition file, wrap the body in the new namespace and drop the prefix:

```cpp
// processor.hpp
namespace tiny::plugin {       // was: namespace tiny
class Processor {              // was: class Plug_processor
    // refer to models as models::Params / models::Meters
};
} // namespace tiny::plugin
```

Framework types (`Set_param`, `Plugin_state`, `Edit_context`, `enum_raw`, …)
remain in `tiny::` and resolve unqualified from inside `tiny::plugin` /
`tiny::models`.

## 4. Address enums → nested `Address`

The per-model address enum is now named `Address` (was `Param_address` /
`Meter_address`), nested in the model:

```cpp
// params.hpp
namespace tiny::models {
struct Params {
    enum class Address : uint32_t { gain = 0, num_params };  // type is now `Address`
};
}
```

In `processor.hpp`/`editor.hpp`, the convenience alias becomes:

```cpp
using Address = models::Params::Address;   // was: using Param_address = Param_model::Param_address;
// usage: enum_raw(Address::gain)
```

**Collision caveat:** a file that needs *both* the param and meter address enums
cannot alias both to `Address`. Keep `Address` for params and qualify the meter
one inline, e.g. `models::Meters::Address::my_meter`.

## 5. Meters API: `make_spec` switch + slimmer `Spec`

The meter model now returns **one spec per address** via a switch, instead of a
vector. `Meter_spec` lost its `address` field (it's keyed by position now). The
sentinel **must** be `Num_meters` (the concept enforces it).

```cpp
// meters.hpp  — BEFORE
struct Meter_model {
    enum class Meter_address : uint32_t { my_meter = 0, num_meters };
    static auto make_specs() -> std::vector<Meter_spec> {
        return { Meter_spec{ .address = enum_raw(Meter_address::my_meter),
                             .range = Lin_range{0,1}, .policy = Meter_policy::stream } };
    }
};

// meters.hpp  — AFTER
namespace tiny::models {
struct Meters {
    enum class Address : uint32_t { my_meter = 0, Num_meters };   // capitalized sentinel
    static auto make_spec(Address address) -> meters::Spec {
        using namespace meters;
        switch (address) {
            case Address::my_meter:
                return { .range = Range{0, 1}, .policy = Policy::Stream };
            default:
                return {};                                         // required (-Wswitch-default)
        }
    }
};
static_assert(meters::Model<Meters>);
}
```

Zero-meter models still need a trivial `make_spec`:
`static auto make_spec(Address) -> meters::Spec { return {}; }`.

## 6. Meters namespace + type renames

Meter types moved to `tiny::meters` and dropped prefixes:

| Old (`tiny::`) | New |
|---|---|
| `Meter_policy` | `meters::Policy` |
| `Meter_policy::peak / stream / trig` | `meters::Policy::Peak / Stream / Trig` (capitalized) |
| `Lin_range` | `meters::Range` |
| `Meter_spec` | `meters::Spec` (now just `{ range, policy }`) |
| `Some_meter_model` (concept) | `meters::Model` |
| `Meter_infos<M>` | `meters::Infos<M>` |
| `Tagged_meter` | `tiny::view::Meter_state` (moved to the view layer; internal) |

`meters::plain_to_norm` / `norm_to_plain` are now VST3-internal — if you were
calling them, switch to your own conversion.

## 7. Format wrapper internals (only if you forked a wrapper)

Plug-in authors normally don't touch these. If you vendored/forked a wrapper:
files dropped their format prefix (`vst3_controller.hpp` → `controller.hpp`,
etc.; `vst3_processor` → `audio_effect`; `auv3_AUAudioUnit` → `audio_unit` file,
class name kept), classes moved into per-format namespaces
(`tiny::vst3::Audio_effect/Controller/View`, `tiny::clap::Plugin/View`,
`tiny::auv2::Effect/View`, `tiny::auv3::View`, `tiny::aax::Parameters/Gui`), and
generated headers dropped prefixes (`aax_categories`→`categories`,
`*_preset_list`→`preset_list`).

**AUv2 entry point changed:** the component factory is now `EffectFactory`
(was `Auv2_effectFactory`). If you maintain a custom AUv2 `Info.plist` /
`exports.txt`, update `factoryFunction` → `EffectFactory` and export
`_EffectFactory`. (Identification is still by AudioComponent codes, so installed
plug-ins / saved projects are unaffected.)

## 8. CMake

- Update your plug-in `CMakeLists.txt` `target_sources` to the new file names (§2).
- `configure_plug_info` / generated `plug_info.hpp` (note `.hpp`) unchanged in role.

---

## Not yet migrated (don't change these yet)

- **Params API:** still `build_tree() -> Param_node`; types
  (`Param_spec`, `Param_group`, `Param_node`, `Real_semantics`, `Adapt_*`,
  `Units`, `Some_param_model`, `Param_infos`, `enum_raw`) remain in flat
  `tiny::`; the sentinel is still lowercase `num_params`. Only the file
  (`params.hpp`), class (`models::Params`), and enum name (`Address`) changed.
- **Worker internals:** `worker.hpp` / `tiny::plugin::Worker` is the only worker
  change so far. The message-type aliases and tuning constants
  (`From_processor`, `reply_capacity`, `poll_interval`, …) keep their current
  shape — the nested `Model` restructure hasn't landed.
- **Framework sub-namespaces** (`tiny::params`, `tiny::events`, `tiny::view`,
  etc.) beyond what's listed above.
