# Plan: The model layer — core/interface split, generated discovery, full optionality

> Status: **design.** A structural direction that sits underneath
> [optional-models.md](optional-models.md), [headless-plugin.md](headless-plugin.md),
> [state-sync-v2.md](state-sync-v2.md) and [buffer-system.md](buffer-system.md).
> It does not replace them — it changes *how* their optionality and their model
> types are wired, and unblocks the typed designs several of them want.
>
> Prototyped end-to-end in two sibling sandboxes (see
> [Prototypes](#prototypes)); every claim below that says "verified" was compiled.

## The direction in one page

tinyplug's premise is that **the author declares a model and the framework
delivers it everywhere**. Today that promise is fully kept for exactly two models
(params, meters), partially for a third (worker), and not at all for the ones the
in-flight plans want to add (state slots, buffers, blocks).

Three things are in the way, and they are the same three thing in different
disguises:

1. **`libs/tinyplug` is one `STATIC` library.** Its TUs are compiled once for the
   whole build with no plug-in on the include path, so **no framework header can
   ever name a model type**. Every design that wants typed model data at the
   framework boundary has to erase it instead.
2. **Discovery happens inside a framework header.** `__has_include("worker.hpp")`
   from [tiny_worker.hpp](../libs/tinyplug/include/tinyplug/tiny_worker.hpp)
   resolves differently depending on *which target* is compiling — which is safe
   today only because of an unwritten rule, and stops being safe the moment a
   framework header becomes model-aware.
3. **Each model is wired differently.** Params/meters are named directly by
   wrappers (18 + 10 sites); the worker goes through a framework alias; the
   worker's *channel shape* is nested inside the user's `Worker` class, so it
   cannot be read without dragging in user logic that needs framework types.

The direction: **make "can this see the model?" a fact about the build graph**,
generate discovery from CMake, and give every model — present and planned — the
same declaration shape.

```
tinyplug_core (STATIC, compiled once)     vocabulary + everything model-free
      |
      v
<plugin>/source/models/*.hpp              author's declarations; core-only
      |
      v
tiny_models.hpp (generated)               models::Bundle + Infos aliases
      |
      v
tinyplug (INTERFACE, header-only)         events, actions, undo, processor, edit, view, worker
      |
      v
<plugin>/source/{processor,editor,worker} + wrappers
      |
      v
tiny_client.hpp (generated)               Client + has_editor / has_worker
```

---

## M1 — Split `libs/tinyplug` into core and interface

### Why this is the enabling move

The hazard is concrete, not theoretical. `undo_history.cpp` includes
`undo_history.hpp` includes `tiny_events.hpp` — a direct chain, no umbrella
involved. The moment `User_action` gains a model-dependent alternative (which
[state-sync-v2.md](state-sync-v2.md) §2.4 needs), that TU compiles one layout in
`libtinyplug.a` while every wrapper compiles another. Same name, same namespace,
different size: **a silent ODR violation**, not a compile error.

The same reasoning applies to `process::Config` carrying typed slots,
`Undo_step` holding slot values, and `Plugin_state` exposing them.

Two clarifications worth recording, because both are easy to get wrong:

- **Changing discovery from `__has_include` to CMake generation does not fix
  this.** The static library still has no plug-in on its include path either way.
  Discovery is a *robustness* question; the library structure is the *capability*
  question. They are independent.
- **Today's `__has_include` for the worker is safe only by discipline.** Verified:
  none of the 11 TUs in `libs/tinyplug/source/` includes
  `<tinyplug/tinyplug.hpp>` — each includes only its own header — and nothing in
  the static library touches `User_worker`. That rule is written down nowhere and
  its failure mode is silent.

Moving the model-aware headers to an `INTERFACE` target removes the hazard **by
construction**: the target has no TUs, so those headers only ever compile inside
a per-plug-in translation unit, where the models are always present.

### `libs/tinyplug_core/` — STATIC, compiled once

Headers (21 moved, 5 new):

| Header | Note |
|---|---|
| `tinyplug_core.hpp` | **new** — core umbrella |
| `tiny_utils.hpp` | |
| `platform_defs.hpp` | |
| `tiny_params.hpp` | vocabulary: `Spec`, `Semantics`, `Adapter`, `Node`, `Model`, `Infos` |
| `tiny_meters.hpp` | |
| `tiny_state.hpp` | **new** — see [M3](#m3--one-model-contract) |
| `tiny_work.hpp` | **new** — split from `tiny_worker.hpp`; channel shape + `No_work` |
| `tiny_input.hpp` | **new** — split from `tiny_view.hpp`; pointer/event vocabulary |
| `tiny_notifications.hpp` | **new** — split from `tiny_events.hpp`; `Set_param`, `Dark_mode_changed`, `Host_preset_loaded`, `Host_event` |
| `state_rules.hpp` | |
| `state_adapter.hpp` | switch to `<nlohmann/json_fwd.hpp>` — see [build times](#build-times) |
| `value_helper.hpp` | |
| `host_formatter.hpp` | |
| `gesture_recognizers.hpp` | core **only after** `tiny_input.hpp` exists |
| `lock_free_queue.hpp` | |
| `change_list.hpp` | drop its incidental `tiny_events.hpp` include (comments only) |
| `meter_mailbox.hpp` | |
| `meter_publisher.hpp` | |
| `notification_queue.hpp` | |
| `serial_queue.hpp` | |
| `task_manager.hpp` | |
| `task_launcher.hpp` | |
| `tiny_log.hpp` | |
| `lifecycle_probe.hpp` | |
| `denormal_guard.hpp` | |
| `window_token.hpp` | |

Sources — 9 of 11, still compiled once:

| Source | Lines |
|---|---|
| `tiny_log.cpp` | 747 |
| `value_helper.cpp` | 429 |
| `gesture_recognizers.cpp` | 346 |
| `state_adapter.cpp` | 185 |
| `host_formatter.cpp` | 145 |
| `task_launcher.cpp` | 68 |
| `task_manager.cpp` | 59 |
| `notification_queue.cpp` | 54 |
| `serial_queue.cpp` | 42 |
| **total** | **2,075** |

### `libs/tinyplug/` — INTERFACE, header-only, model-aware

| Header | Change |
|---|---|
| `tinyplug.hpp` | umbrella; includes generated `<tiny_models.hpp>` |
| `tiny_events.hpp` | `User_action` gains typed `Set_state`; keeps `Ui_receiver` |
| `action_queue.hpp` | absorbs `action_queue.cpp` (97 lines) |
| `undo_history.hpp` | absorbs `undo_history.cpp` (254 lines) |
| `tiny_processor.hpp` | `Config` carries typed slot values |
| `tiny_edit.hpp` | `Plugin_state` carries typed slot values |
| `tiny_view.hpp` | `Processor_state`, `Plugin_state`, `run_frame`, `Draw_callback`, `Notify_callback` |
| `tiny_worker.hpp` | actors, runner, reply-detection concepts |

**Sources: none.** 351 lines become header-only.

### The three files that split rather than move

| File | → core | → interface |
|---|---|---|
| `tiny_view.hpp` | `Coords`, `Frame`, `Rect_size`, `Modifier_keys`, `Pointer_button`, all `Pointer_*`, `Pointer_event`, `Event`, `Event_list`, `Event_stream`, `User_interaction`, `View_context`, `Scroll_data`, `Durations`, clocks | `Processor_state`, `Plugin_state`, `run_frame`, `Draw_callback`, `Notify_callback` |
| `tiny_events.hpp` | `Set_param`, `Dark_mode_changed`, `Host_preset_loaded`, `Host_event` | `Action_start`, `Action_end`, `Request_resize`, **`Set_state`**, `User_action`, `Ui_receiver` |
| `tiny_worker.hpp` | channel-shape concept, `No_work` | `Worker_actor`, `Worker_reply_actor`, `Worker_runner`, reply concepts |

`Ui_receiver` lands in the interface half because its `Action_handler` is
`std::function<void(const User_action&)>`. `Host_preset_loaded` lands in core
because its payload is `std::span<const Set_param>` — address and double, no
model types — and `tiny_platform` needs `Dark_mode_changed` from the same file.

### The `tiny_view.hpp` split is the load-bearing one

It is what keeps **`gesture_recognizers.cpp` (346 lines) compiled once.** The
header uses only `Pointer_button` and the pointer/timing vocabulary; it never
touches `Plugin_state` or `User_action`. Without the split it gets dragged into
the interface by a single `#include "tiny_view.hpp"`.

### `tiny_platform` validates the seam

It currently includes `<tinyplug/tinyplug.hpp>` and is itself a `STATIC` lib, so
it would inherit the same hazard. But an audit of every framework type it names
returns:

```
29 Coords   17 Window_token   14 Rect_size   14 Pointer_button
 7 Pointer_down   6 Dark_mode_changed   5 User_interaction   5 Pointer_up ...
```

**Not one model-aware type.** After the `tiny_input` / `tiny_notifications`
splits it repoints at `<tinyplug_core/tinyplug_core.hpp>` and is cleanly
core-only. The platform layer already lived entirely on the core side; it just
wasn't expressible.

---

## M2 — Generated discovery replaces `__has_include`

Two generated headers, one per directory level. `configure_plug_info` already
generates `plug_info.hpp` from target properties, so the machinery exists.

| Generated | Includes | Declares |
|---|---|---|
| `tiny_models.hpp` | `tinyplug_core` + `models/*.hpp` | `models::Bundle`, `User_params` / `User_meters` / `User_slots` / `User_work`, `has_*` |
| `tiny_client.hpp` | `tinyplug` + `processor/editor/worker.hpp` | `Client`, `has_editor`, `has_worker` |

```cpp
// tiny_models.hpp -- GENERATED
#include <tinyplug_core/tinyplug_core.hpp>
#include "models/params.hpp"
#include "models/meters.hpp"
#include "models/state.hpp"          // or omitted
#include "models/work.hpp"           // or omitted

namespace tiny::models {
struct Bundle {
    using Params = models::Params;   // or params::No_params
    using Meters = models::Meters;   // or meters::No_meters
    using State  = models::State;    // or state::No_state
    using Work   = models::Work;     // or work::No_work
};
}

namespace tiny {
static_assert(params::Model<models::Bundle::Params>);
static_assert(meters::Model<models::Bundle::Meters>);
static_assert(state::Model<models::Bundle::State>);
static_assert(work::Model<models::Bundle::Work>);

using User_params = params::Infos<models::Bundle::Params>;
using User_meters = meters::Infos<models::Bundle::Meters>;
using User_slots  = state::Infos<models::Bundle::State>;
using User_work   = models::Bundle::Work;

inline constexpr bool has_params = !std::is_same_v<models::Bundle::Params, params::No_params>;
// ... has_meters / has_state / has_work
}
```

What this buys over `__has_include`:

- **No ordering rule.** The "must come last" comment at the bottom of
  [tinyplug.hpp](../libs/tinyplug/include/tinyplug/tinyplug.hpp) exists solely so
  a user header can see framework types. With generation there is nothing to
  order — the generated header is included after core is complete, always.
- **No quoted-include ambiguity.** `__has_include("worker.hpp")` searches the
  *includer's* directory first (i.e. `libs/tinyplug/include/tinyplug/`) before
  the plug-in's `source/`. A generic filename could silently resolve wrong.
- **Concept violations are diagnosed at the generated header**, not 200 lines
  deep in a wrapper.
- **One place to add a model.** A fifth model touches one generated template,
  not fourteen wrapper headers.
- **It collapses the alias migration [optional-models.md](optional-models.md)
  Part 2a already schedules.** Those 18 + 10 sites move to `User_params` /
  `User_meters` regardless; generating them means the migration lands once and the
  optionality comes free.

The one honest cost: adding or removing a model header requires a CMake
re-configure. That is already true of adding a source file.

### Direction is convention, not structure

A model header that includes `<tinyplug/tinyplug.hpp>` is a benign no-op cycle
while it only uses core types, and fails with a plain
`no type named 'Action_queue' in namespace 'tiny'` the moment it reaches for an
interface type. Both verified. The failure mode is decent but it is not enforced.
If it ever matters, a CMake check that no `models/*.hpp` mentions the `<tinyplug/`
include root would make it structural. Not worth building yet.

---

## M3 — One model contract

Every model — present and planned — takes the same shape:

```cpp
struct <Name> {
    enum class Address : uint32_t { ..., Num_<things> };
    static constexpr auto make_spec(Address) -> <ns>::Spec;
};
```

`Infos<M>` caches `std::array<Spec, N>` at static init and exposes `spec(i)`.
`meters::Model` is already exactly this; `params::Model` uses `build_tree()`
because it needs display hierarchy, which is a real difference and stays.

### `state::Model` — the worked example

Design settled during this session after trying three alternatives (a `Slots`
tuple, aggregate reflection with an arity probe, and a `type_for` returning a
default value). The winner is the one that matches the framework's own idiom:

```cpp
// core -- tiny_state.hpp
namespace tiny::state {

enum class Policy : uint32_t { Persist = 0, Session, Transient };

struct Spec {
    std::string_view identifier{};   // or std::string -- see below
    std::size_t kind{};              // index into the model's Types
    Policy policy{Policy::Persist};
};

template<typename T, typename V> inline constexpr auto kind_of = Index_of<T, V>::value;

template<typename T> concept Model = requires(typename T::Address a) {
    typename T::Address; typename T::Types;
    { T::Address::Num_items } -> std::same_as<typename T::Address>;
    { T::make_spec(a) } -> std::same_as<Spec>;
};
}
```

```cpp
// author -- source/models/state.hpp
struct State {
    enum class Address : uint32_t { Pattern_a, Pattern_b, Order, Num_items };
    using Types = std::variant<Pattern, Chain_order>;

    static constexpr auto make_spec(Address address) -> state::Spec
    {
        using enum Address;
        switch (address) {
        case Pattern_a: return {"pattern_a", state::kind_of<Pattern, Types>};
        case Pattern_b: return {"pattern_b", state::kind_of<Pattern, Types>};
        case Order:     return {"order", state::kind_of<Chain_order, Types>,
                                state::Policy::Session};
        case Num_items: break;   // Not `default:` -- that disables -Wswitch.
        }
        return {};
    }
};
```

**`kind` does four jobs**, which is why it is an index rather than a size:

| Job | Mechanism |
|---|---|
| default value | `variant_alternative_t<kind, Types>{}` — honours in-class initialisers, so defaults need not be zero |
| compile-time slot type | `Slot_type<A> = variant_alternative_t<make_spec(A).kind, Types>` |
| wrong-slot guardrail | `Store::set` compares `value.index()` to `spec.kind` |
| layout-drift rejection | load checks kind + version + size, falls back to the default rather than memcpy'ing a stale layout |

Three findings from the prototype worth keeping:

- **A `size()` accessor is neither necessary nor sufficient.** Size is derivable
  from the alternative, and a byte count cannot pick a variant alternative or
  produce a non-zero default.
- **`kind` is what lets `Spec` live in the framework namespace.** A `Types
  default_value` field would embed a model type, forcing `Spec` to be nested in
  the model or templated on it — it could not sit beside `params::Spec` and
  `meters::Spec`. It also keeps `sizeof(Spec)` constant instead of tracking the
  largest slot.
- **Two slots may share a type.** `Types` is a set of distinct payload types, not
  a slot list; `Grid, Grid, Chain` is expressed by two addresses mapping to the
  same `kind`.

Per-slot defaults (two slots, same type, different starting values) are
recoverable with an **opt-in, concept-detected** hook rather than a fatter `Spec`:

```cpp
template<typename M> concept Has_slot_defaults = requires(typename M::Address a) {
    { M::make_default(a) } -> std::same_as<typename M::Types>;
};
// Infos::default_for(a) uses the hook when present, else the type's own default.
```

### The work model lifts out of the `Worker` class

`Worker::Model` currently nests the four channel variants inside the user's
`Worker` class, so reading the shape means including a header that needs
`Worker_reply_actor` and `Task_manager::Actor` — interface types. That is exactly
why the worker needs the "must come last" rule.

Split it the same way state splits:

- `models/work.hpp` — the four channel variants plus capacities. **Core only.**
- `worker.hpp` — the `Worker` class. **May use tinyplug proper.**

The generated model header then reads the shape without dragging in user logic,
and the worker stops being the odd one out. CMake enforces the pairing:
`worker.hpp` without `models/work.hpp` is a configure-time error, not a template
failure.

### Fallback models must satisfy the full contract

The prototype's `No_state` originally used `std::variant<std::monostate>` and the
build broke for a plug-in with no state model:
`no member named 'version' in 'std::monostate'`, because the erase/rehydrate path
reads `T::version`. It now carries a real empty slot type with `version = 0`.

**A monostate escape hatch is not enough.** This is a direct amendment to the
pattern [optional-models.md](optional-models.md) describes, and it applies to
`No_params`, `No_meters` and `No_work` too — each should be a real zero-entry
model, not a monostate. The empty case is a case; it needs a fixture in CI.

### Optionality matrix

| File | Absent ⇒ | Level |
|---|---|---|
| `models/params.hpp` | `params::No_params` (zero entries) | models |
| `models/meters.hpp` | `meters::No_meters` | models |
| `models/state.hpp` | `state::No_state` | models |
| `models/work.hpp` | `work::No_work` | models |
| `editor.hpp` | `edit::No_editor`, `has_editor == false` | client |
| `worker.hpp` | `No_worker`, `has_worker == false` | client |
| `processor.hpp` | — required | client |

Every optional branch in wrapper code must sit **inside a template**: in a
non-template function `if constexpr (false)` still performs name lookup on the
discarded branch. This is the constraint CLAUDE.md already documents for worker
reply drains; once everything is optional it applies to far more surface. The
prototype routes `draw`, `emit_gesture`, `apply_state`, `exercise_worker` and
`report_density` through function templates for this reason.

---

## Relationship to the existing plans

These plans are **not modified**. This section records where this direction
changes an assumption in each.

### [optional-models.md](optional-models.md)

- **Part 1 (zero-parameter trees) stands unchanged and is a prerequisite.** L1
  (the AAX render queue missing the bypass, fatal at zero params) is a real crash
  and should land first regardless.
- **Part 2a's `__has_include` discovery is superseded by M2.** The `User_params` /
  `User_meters` aliases it introduces are exactly what `tiny_models.hpp`
  generates, and the 18 + 10 site migration is the same work — so Part 2a lands
  as "generate the aliases" rather than "sniff for the header".
- Its open question — *"decide whether the trigger is `models/params.hpp` or a
  flat `params.hpp`"* — resolves to **`models/params.hpp`**, discovered by CMake,
  matching the existing convention.
- 2b (optional meters) and 2c (optional params) are unaffected in substance.
- Amendment: the stubs must be zero-entry models, not monostates (above).

### [headless-plugin.md](headless-plugin.md)

- Its central insight — *"CMake is the single source of truth"*, because only
  CMake can exclude view sources and drop the platform link — is **generalized
  here to every model.** Headless got there first for a link-time reason; this
  plan arrives at the same place for an ODR reason.
- Its remark that the worker *"has no link-time footprint and so can stay purely
  `__has_include`"* no longer holds once framework headers become model-aware.
  The worker moves to generated discovery with everything else.
- The `TINY_HEADLESS` override, the per-format view/link gating, and the AUv3
  empty-view-controller decision are all unaffected and still needed — this plan
  supplies `has_editor` / `Client::Editor` from `tiny_client.hpp`, but the
  invasive CMake work in that plan is the real content and is unchanged.

### [state-sync-v2.md](state-sync-v2.md)

Written earlier in the same session; this supersedes three parts of it:

- **§2.1** proposed a `Slots` tuple parallel to `Address`. Superseded by
  `make_spec` returning `state::Spec` with `kind` (M3).
- **§2.4** proposed `Blob_ref` + an arena because `tiny_events.hpp` could not name
  model types. After M1 it *can*, so a typed `Set_state{Address, Types}` is
  available. **The arena survives as a performance option, not a constraint** —
  an inline `Types` makes `sizeof(User_action)` track the largest slot
  (measured: 120 bytes for a 104-byte `Types`; ~900 per undo change with a
  realistic 448-byte `Pattern`).
- **§2.5's** version mechanism stands; the drift check gains the `kind` mismatch
  test.
- Everything else — transactional publish, the meter snapshot, `Value_store`,
  `State_document`, plain-space/double canonicalisation, the v1→v2 bridge — is
  unaffected.

### [buffer-system.md](buffer-system.md)

- Its **"Why not one serializer"** rejection was correct on its own terms (don't
  drag JSON/DOM cost onto the host-session hot path) and this plan preserves both
  encoders. What changes is the layer *above* them.
- Its exclusion — *"No generic non-audio blob system. Sequences/patterns are
  params"* — is reversed by `state::Model`, and its "Relationship to the rest of
  the framework" table needs a `state::Model` row.
- Its stated dependency — *"push/remove are discrete undoable actions the editor
  records in the existing `Undo_history`"* — is **currently not implementable**:
  `Undo_step` holds only `Param_change` and `process_actions` accepts only a
  `User_action` span. M1 plus state-sync-v2 §2.4 is what makes it true.
- The merge shape: a `Buffer_source` descriptor becomes a state slot (it is a
  POD — a path is `std::array<char, N>`), while the *bytes* stay with the buffer
  system's prepare / install / retire machinery. Most of its persistence section
  then collapses.
- **Sequencing caveat:** this couples the September looper slice to a model that
  does not exist. Build the looper against a hand-rolled descriptor and migrate;
  do not block the vertical slice.

---

## Build times

Measured on this machine, single TU, best of three.

| Umbrella variant | Preprocessed lines | Compile |
|---|---|---|
| today | 129,087 | 0.92s |
| `json_fwd` in `state_adapter.hpp` | 103,284 | 0.71s |
| `state_adapter.hpp` removed entirely | 99,663 | 0.61s |
| `<nlohmann/json.hpp>` alone | 119,354 | 0.62s |

**tinyplug's own headers are ~10k lines.** The ~100k floor is libc++. nlohmann is
92% of the delta, and it reaches every wrapper TU through
`tinyplug.hpp → tiny_edit.hpp → state_adapter.hpp`.

Three conclusions:

1. **The split is build-time neutral.** It adds 351 lines of definitions to TUs
   already preprocessing 100–129k — about +0.3% — and removes two TUs from the
   compiled-once set.
2. **`json_fwd` is worth doing on its own**, independent of everything here. The
   multiplier is 24 wrapper TUs × 7 example plug-ins = 168 wrapper compilations
   per full build. Caveat: seven files
   (`aax/parameters.hpp`, `auv2/effect.{hpp,cpp}`, `vst3/controller.hpp`,
   `auv3/audio_unit.mm`, `clap/plugin.{hpp,cpp}`) call functions returning
   `ordered_json` **by value** and need the full header locally. The other ~17
   save. Note `json_fwd` is itself 74k lines — it captures about two-thirds of the
   available win, not all of it.
3. **PCH is worth more than any of this.** The floor is the standard library, so a
   precompiled header on the umbrella collapses ~0.6s for all 168 compilations.
   Already on the backlog in [refactor-ideas.md](refactor-ideas.md); if build
   times are the pain, do that first — it is orthogonal to the split.

A further ~0.10s is available by getting `state_adapter.hpp` out of
`tiny_edit.hpp`, which includes it for exactly one member
(`State_adapter::Actor state_adapter{}`). Forward-declaring the `Actor` or moving
it to its own header removes nlohmann from the editor path entirely.

---

## Prototypes

Two sibling sandboxes, both building and running:

- **`../tiny_state`** — single `STATIC` lib, mirroring today's structure. Shows
  the model contract with erasure at the boundary: `Slot_view`, type-erased
  persistence, `Store<Model>` as a template.
- **`../tiny_state2`** — the core/interface split. Same wrapper source compiles
  two plug-ins: `seq_demo` (params + meters + state + work, editor + worker) and
  `headless_demo` (params only, no editor, no worker). Typed `Set_state`, typed
  `Config` slots, typed `Undo_step`, `Store` as a plain class.

Each has a README with a "things to try" list. The most instructive: move
`undo_history.hpp` back into a `.cpp` in the core lib and watch it fail — that is
the constraint the whole split exists to remove.

---

## Staging

Steps 1–3 are pure refactors with no structural change, which keeps the risky
part small and late.

| # | Work | Depends on | Size |
|---|---|---|---|
| 1 | Split `tiny_view.hpp` → `tiny_input.hpp` + `tiny_view.hpp`; drop `change_list`'s incidental include | — | M |
| 2 | Split `tiny_events.hpp` → `tiny_notifications.hpp` + `tiny_events.hpp` | — | S |
| 3 | Split `tiny_worker.hpp` → `tiny_work.hpp` + `tiny_worker.hpp`; lift `Work` out of the user's `Worker` class into `models/work.hpp` | — | M |
| 4 | `json_fwd` in `state_adapter.hpp` + seven local includes | — | S |
| 5 | Create `tinyplug_core`; move the core set; `tinyplug` becomes `INTERFACE`; repoint `tiny_platform` | 1–3 | L |
| 6 | `action_queue.cpp` / `undo_history.cpp` → headers | 5 | S |
| 7 | `configure_models` + `configure_client`; generated `tiny_models.hpp` / `tiny_client.hpp`; retire `__has_include`; migrate the 18 + 10 alias sites | 5 | L |
| 8 | `No_params` / `No_meters` / `No_work` as zero-entry models; optionality fixtures in CI | 7 | M |
| 9 | `state::Model` — core `tiny_state.hpp`, `Store`, transport, persistence | 7 | L |
| 10 | Make `User_action` / `Config` / `Plugin_state` model-aware — the payoff | 7, 9 | M |

Steps 1–4 deliver value with no structural change at all. Step 5 is the pivot.
Step 10 is inert until 5 lands, which is why it is last.

---

## Open questions

- **O1.** `state::Spec::identifier` — `std::string_view`, `std::string`, or
  `std::array<char, N>`? All three work. `std::string` is viable because
  `make_spec(A).kind` reads a *temporary* inside the constant evaluation, so the
  allocation is transient — verified, including past SSO. The trap: a `Spec` can
  be **read** in a constant expression but never **stored** as one
  (`static constexpr auto s = make_spec(A);` fails with
  *"pointer to subobject of heap-allocated object"*). Choose `std::string` if
  computed identifiers matter (sixteen pattern slots named in a loop),
  `std::array<char, N>` if constexpr-storable Specs ever matter.
- **O2.** Should `params::Model` migrate to `make_spec` for symmetry, or keep
  `build_tree()`? It genuinely needs display hierarchy, so probably keep — but
  the asymmetry is the one wrinkle in "every model reads the same".
- **O3.** Does the interface library want its own namespace or `detail/` split
  once it is header-only? Relates to the `detail/` item in
  [refactor-ideas.md](refactor-ideas.md).
- **O4.** Is a CMake check enforcing "no `models/*.hpp` includes `<tinyplug/`"
  worth the machinery, or is the compile error good enough?
- **O5.** `headless_demo` in the prototype is an `INTERFACE` target with no TUs,
  so it contributes nothing to `compile_commands.json` and its headers get
  degraded IntelliSense. Should a plug-in target be required to have at least one
  TU?

---

## Explicitly not in scope

- Any change to the latency protocol, event ordering, or the audio-thread
  allocation rules. Everything here is compile-time structure.
- The `params::Node` tree, the three value spaces, or `Value_helper`.
- Anything in [state-sync-v2.md](state-sync-v2.md) beyond the three superseded
  sections named above.
- Runtime model registration. Every model stays compile-time and append-only.
