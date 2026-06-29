# Plan: Optional models — zero-parameter trees and fully optional meters/params

> Status: **design.** Two related goals, sequenced as parts:
>
> - **Part 1 — zero-parameter trees.** Make a plug-in with *no user
>   parameters* (an empty param tree) a fully supported, validated
>   configuration across all five wrappers, the preset model, and the state
>   adapter. Audit and fix the limitations that an empty tree exposes, and
>   tighten other latent tree-validation gaps found along the way.
> - **Part 2 — full model optionality.** Make the **meter model** and **param
>   model** opt-in the same way the worker is today (`__has_include` +
>   monostate stub ⇒ zero overhead when absent), so a plug-in that declares no
>   meters carries no meter queues/plumbing, and one that declares no params
>   carries no param machinery beyond the framework-injected bypass.
>
> Part 1 is a prerequisite for the param half of Part 2: "no param model"
> resolves to an empty tree, so every empty-tree code path must already be
> correct before the stub is wired in. This plan follows the precedent set by
> [headless-plugin.md](headless-plugin.md) (optional editor) and the existing
> optional worker ([tiny_worker.hpp](../libs/tinyplug/include/tinyplug/tiny_worker.hpp)).

## Background: how the worker does it today

The worker is the reference pattern for "absent ⇒ zero overhead"
([tiny_worker.hpp](../libs/tinyplug/include/tinyplug/tiny_worker.hpp)):

- `#if __has_include("worker.hpp")` picks `plugin::Worker`, else aliases a
  `No_worker` **monostate** stub. `TINY_HAS_WORKER` (preprocessor) lets wrappers
  `#if`-gate member declarations out entirely; `inline constexpr bool has_worker`
  gates code inside templated helpers where `if constexpr` properly discards.
- Every worker channel collapses to `std::monostate`; queues/threads/storage
  vanish; reply handlers are concept-detected inside *templates* so the
  undetected branch is discarded in a dependent context.

The crucial difference for params/meters: **the framework currently does not
choose the model type — user code names it directly.** Wrappers hardcode
`tiny::models::Params` / `tiny::models::Meters` and `params::Infos<models::Params>`
(18 + 10 reference sites across the wrappers, each `#include "models/params.hpp"` /
`"models/meters.hpp"`). The worker, by contrast, is referenced only through the
framework alias `User_worker`. So Part 2 requires introducing `User_params` /
`User_meters` framework aliases and migrating the wrappers onto them — see Part 2.

---

# Part 1 — Zero-parameter trees

## What already works

- **Model concept.** `params::Model`
  ([tiny_params.hpp:273](../libs/tinyplug/include/tinyplug/tiny_params.hpp#L273))
  only requires `Address::Num_params` and `build_tree()`. A user can legally
  declare `enum class Address : uint32_t { Num_params };` (value 0) and
  `build_tree()` returning `Group{}`.
- **Validation already allows it.**
  `impl::validate_tree`
  ([tiny_params.hpp:316](../libs/tinyplug/include/tinyplug/tiny_params.hpp#L316))
  explicitly early-returns `true` when `num_leaves == 0` (skipping the
  dense-0..n−1 checks). This was clearly anticipated.
- **Bypass keeps the host happy.** Every format injects a framework-owned
  bypass parameter, so the host never actually sees *zero* parameters even when
  the user tree is empty:
  - AAX — `cDefaultMasterBypassID`, registered as a synchronized parameter
    ([parameters.cpp:179](../wrappers/aax/source/parameters.cpp#L179)).
  - CLAP — `paramsCount() == num_params + 1`, bypass at the end
    ([plugin.cpp:788](../wrappers/clap/source/plugin.cpp#L788)).
  - VST3 — `bypass_param_id` ([audio_effect.cpp:623](../wrappers/vst3/source/audio_effect.cpp#L623)).
  - AUv2 / AUv3 — bypass via `_bypass` (`Host_bypass`).

  This neutralizes the "some hosts crash with zero automatable parameters"
  concern: bypass is automatable and always present.
- **Zero-size storage is legal.** `std::array<T, 0>` for `_ui_params`,
  `_last_points`, `_host_load_after`, the `make_defaults` array, etc. is
  well-formed; none of the wrappers index `[0]` / `.front()` unconditionally
  (verified by grep — no hits). The `4 * num_params + 1` state-queue sizes all
  evaluate to `1` (non-empty) because of the `+ 1`.

## Limitations to fix

### L1 — AAX render queue undersized at zero params *(real defect)*

[monolith.hpp:318](../wrappers/aax/source/monolith.hpp#L318) sizes the
synchronized-parameter render queue from the user param count only:

```cpp
static constexpr auto min_queue_size = params::Infos<models::Params>::num_params;
// ... SParamValList::sCap = 4 * min_queue_size;  →  TParamValPair* mElem[sCap];
```

This is a **latent off-by-one**: the bypass param *is* a synchronized parameter
(`AddSynchronizedParameter(*bypass_param)`,
[parameters.cpp:191](../wrappers/aax/source/parameters.cpp#L191)) but is **not**
counted in `min_queue_size`. With ≥1 user param the slack has hidden it; at zero
params it becomes fatal — `sCap == 0`, so `TParamValPair* mElem[0]` is a
zero-length array (non-standard; UB to ever `Add()` into), and the bypass update
queued during `SetChunk()` / param change has nowhere to go.

**Fix.** Count the bypass: `min_queue_size = num_params + 1` (matching the
already-correct `to_processor_size = 4 * num_params + 1` in
[parameters.hpp:176](../wrappers/aax/source/parameters.hpp#L176)). Keep the
`4×` multiplier the SDK comment justifies. Add a `static_assert(min_queue_size >= 1)`.

### L2 — Allow the root node to be a `Spec` (make it a supported configuration)

`build_tree()` returns a `params::Node` (`variant<Group, Spec>`,
[tiny_params.hpp:233](../libs/tinyplug/include/tinyplug/tiny_params.hpp#L233)),
so a single-parameter plug-in returning a bare `Spec` (no enclosing `Group`) is
*legal by the type* — but today it is silently mishandled, because every
consumer assumes the root is a `Group`:

- State adapter dereferences `std::get_if<params::Group>(model.param_tree)` on
  both save and load ([state_adapter.cpp:42](../libs/tinyplug/source/state_adapter.cpp#L42),
  [:119](../libs/tinyplug/source/state_adapter.cpp#L119)) — a root `Spec` falls
  through and produces an empty preset.
- AUv3 inlines `std::get_if<params::Group>(&tree)`
  ([audio_unit.mm:186](../wrappers/auv3/source/extension/audio_unit.mm#L186)).
- The per-format hierarchy walkers (`tree_to_aax_ids`, `tree_to_clump_map`,
  `tree_to_units`, `tree_to_clap_modules`) descend from a root group.

We want a root `Spec` to be a **first-class supported configuration**, not just
the empty `Group{}` case.

**Doability: easy — a ~5-line change in one place.** The decisive fact is that
*every* consumer reads the tree through `User_params::param_tree()`, never
`build_tree()` directly (verified by grep: all wrapper sites call
`param_tree()`; the only raw `build_tree()` call is inside `Infos` itself at
[tiny_params.hpp:396](../libs/tinyplug/include/tinyplug/tiny_params.hpp#L396)).
And `impl::flatten_tree`
([tiny_params.hpp:287](../libs/tinyplug/include/tinyplug/tiny_params.hpp#L287))
*already* handles a root `Spec` (its `std::visit` has both a `Spec` and a
`Group` arm), so `display_specs` / `indexed_specs` are already correct.

So the fix is to **normalize once in `Infos`**: when `build_tree()` returns a
`Spec`, wrap it in a synthetic top-level `Group` before storing `user_tree`:

```cpp
inline static const Node user_tree = [] {
    auto root = User_model::build_tree();
    if (std::holds_alternative<Spec>(root))
        return Node{Group{.nodes = {std::move(root)}}};
    return root;
}();
```

That single change makes the state adapter, AUv3, and all four hierarchy
walkers see a `Group` root unconditionally — **zero per-consumer edits**, fully
backward-compatible (existing plug-ins already return `Group`), and it removes
the silent-corruption footgun. The synthetic group has an empty `name` /
`string_id`, so it contributes no host-visible unit/clump/module.

Rejected alternative: tighten the concept to `{ build_tree() } -> Group`. That
*forbids* the bare-`Spec` ergonomics we want to support and is a breaking
signature change — the opposite direction.

Document the supported shapes (bare `Spec`, root `Group`, empty `Group{}`) in
[CLAUDE.md](../CLAUDE.md) under "Three spaces", and add a single-`Spec`-root
case to the validation/demo coverage.

### L3 — `string_id` uniqueness / non-emptiness unvalidated

`validate_tree` checks address density/uniqueness but never validates
`string_id`. Yet `string_id` is the **preset key** (state adapter writes
`json[spec.string_id]`) and the AUv3/CLAP/AAX hierarchy key. A duplicate or
empty `string_id` silently collides or drops params on preset save/load — a
sharper failure mode than anything zero-params introduces, and worth fixing in
the same validation pass.

**Fix.** Extend `validate_tree` to assert, per sibling scope, that every
persistent param/group `string_id` is non-empty and unique among siblings.
Gate behind the existing one-time `param_tree()` validation so it costs nothing
at runtime.

### L4 — Empty / degenerate groups in host hierarchies

With zero params the root group is empty; with sparse trees an author may leave
an empty subgroup. The state adapter already skips empty subgroups on save
(`if (!subjson.empty())`, [state_adapter.cpp:34](../libs/tinyplug/source/state_adapter.cpp#L34)),
but the wrappers build host-native hierarchy nodes (AUv2 `Clump_map`, VST3
units, CLAP modules from `tree_to_clap_modules`, AAX page tables) directly from
the tree.

**Fix.** Audit each wrapper's tree→hierarchy builder to ensure an empty root
group (and empty subgroups) produce *no* degenerate unit/clump/module and no
out-of-range default-group references. Most loops are naturally empty-safe;
this is a verification + targeted-guard task, not a redesign. Confirm AAX
`describe.cpp` / page-table generation tolerates an empty tree (only bypass
present).

### L5 — Documentation & a regression demo

Zero params is a supported configuration only if it's tested. Add a minimal
demo (e.g. `examples/passthrough/`) with `enum class Address { Num_params };`
and `build_tree()` returning `Group{}`, wired into the CI example set
([examples/CMakeLists.txt](../examples/CMakeLists.txt)). This both documents the
pattern and guards L1–L4 against regressions across all formats. (It also
becomes the natural fixture for Part 2's "no param model" path.)

## Part 1 deliverables

1. Fix L1 (AAX queue `+1` for bypass) — the one concrete crash.
2. Implement L2 (normalize a root `Spec` into a synthetic `Group` in `Infos`,
   making a bare-`Spec` root a supported shape).
3. Extend `validate_tree` for L3 (string_id rules).
4. Audit + guard L4 (empty groups in all five hierarchy builders).
5. Add the `passthrough` zero-param demo (L5) and document the contract in
   CLAUDE.md.

---

# Part 2 — Fully optional meter and param models

Goal: declaring no meters / no params should cost nothing, exactly like the
worker — no `Set_meter` queue, no meter plumbing, no per-format export
parameters when there are no meters; no user-param machinery (beyond bypass)
when there are no params. The user simply omits the model file.

## 2a — The discovery + alias change (shared)

Today wrappers hardcode `models::Params` / `models::Meters`. Mirror the worker:

- In [tiny_params.hpp](../libs/tinyplug/include/tinyplug/tiny_params.hpp), add a
  discovery tail:
  ```cpp
  #if __has_include("params.hpp")          // user model, by convention
      #include "params.hpp"
      #define TINY_HAS_PARAMS 1
      namespace tiny { using User_params = models::Params; }
  #else
      #define TINY_HAS_PARAMS 0
      namespace tiny { using User_params = No_params; }   // stub below
  #endif
  inline constexpr bool has_params = (params::Infos<User_params>::num_params > 0);
  ```
  with a `No_params` stub: `enum class Address : uint32_t { Num_params };`
  (value 0) and `static auto build_tree() -> Node { return Group{}; }`. Same for
  meters with `No_meters` (`Num_meters == 0`, trivial `make_spec`).
- **Migrate wrappers** off `models::Params`/`models::Meters` onto
  `tiny::User_params`/`tiny::User_meters` and `Infos<User_params>` /
  `Infos<User_meters>` (18 + 10 sites). Mechanical but touches every wrapper;
  do it as one rename commit per concern so it's reviewable.
- **Naming/convention.** The current convention nests models under
  `models/params.hpp` (`tiny::models::Params`). Decide whether the discovery
  trigger is `models/params.hpp` or a flat `params.hpp` next to `worker.hpp`
  (the worker uses a flat `worker.hpp`). Recommend keeping `models::Params` as
  the type name but discovering whichever path the scaffold emits; align
  [tools/new_plugin.py](../tools/new_plugin.py) and `template/` accordingly.

The processor/editor that today write `params::Infos<models::Params>` should
also move to `params::Infos<User_params>` so a plug-in that omits the file still
compiles (they get the empty stub).

## 2b — Optional meters (the clean half)

Meters are isolated and value-typed, so this is close to the worker in
difficulty. With `has_meters == false`, gate out:

- **The meter queue.** `Set_meter` lock-free queue (processor→editor; in VST3 the
  output-parameter channel) — `#if`-gate the member out so there is no storage.
- **`run_frame` meter drain.** The meter-draining block and `_ui_meters`
  accumulation in [tiny_view.hpp:327](../libs/tinyplug/include/tinyplug/tiny_view.hpp#L327)
  become `if constexpr (has_meters)`-discarded; `_ui_meters` is a zero-size
  array (already free) but the `pop_meter` loop and `meter_arr` allocation
  should compile out.
- **`Dsp_context::meters`.** The span stays (cheap) but the wrapper's
  meter-array setup is skipped.
- **VST3 export parameters.** The biggest removable cost: skip registering the
  `export_param_offset` read-only parameters in
  [controller.cpp:172](../wrappers/vst3/source/controller.cpp#L172), skip the
  `setParamNormalized` meter-range branch
  ([controller.cpp:616](../wrappers/vst3/source/controller.cpp#L616)), and skip
  writing `data.outputParameterChanges` for meters
  ([audio_effect.cpp:429](../wrappers/vst3/source/audio_effect.cpp#L429)).
- **Other formats** (AAX/AUv2/AUv3/CLAP) similarly skip meter registration/poll.

`No_meters` already makes `num_meters == 0`, so much of this is *already*
zero-cost via empty arrays. The point of Part 2b is to remove the **queues and
code paths**, not just the arrays, and to let the user omit the file. Net: a
self-contained, low-risk change once 2a lands.

## 2c — Optional params (subtler)

"No param model" does **not** mean "no host parameters" — bypass remains. So the
param-optional path is mostly: resolve `User_params` to the empty-tree stub and
rely on **Part 1** having made the empty tree correct everywhere. Specifically:

- `has_params == false` ⇒ the stub's empty `Group{}` flows through the same code
  Part 1 hardened (L1–L4). There is little *additional* gating to do beyond 2a,
  because the wrappers must already handle `num_params == 0`.
- Where worker-style `#if`-gating *does* help: any param-only member that is
  pure overhead at zero (e.g. the state-load `Lock_free_queue<Set_param>` is
  still needed for bypass/preset, so keep it; but `_last_points`,
  `_host_load_after`, the `_snapshot_knob_params` arrays are already zero-size).
  Audit for anything allocating proportional to a *minimum* of 1.
- Keep `has_params` defined as `num_params > 0` (not just "file present"), so a
  plug-in that declares an empty model and one that omits the file behave
  identically.

Because bypass and preset/state plumbing must survive even with zero params, the
param model is **less** removable than meters — the win is author ergonomics
(omit the file) plus the correctness work in Part 1, more than runtime savings.

## Part 2 deliverables

1. **2a:** `No_params` / `No_meters` stubs + `__has_include` discovery +
   `User_params` / `User_meters` aliases; migrate all wrapper reference sites;
   update scaffold/template.
2. **2b:** gate meter queues + per-format meter plumbing behind `has_meters`
   (VST3 export params the headline item).
3. **2c:** verify the param stub rides Part 1's empty-tree path; gate only the
   genuinely param-proportional overhead, preserving bypass/preset/state.
4. Extend the `passthrough` demo into the no-model fixtures: one variant with no
   `meters.hpp`, one with neither model, built across all formats in CI.

## Sequencing & risk

1. **Part 1 first** (L1 is a real crash; the rest hardens the empty tree).
2. **2a** (alias + migration) — mechanical, enables the rest.
3. **2b** (meters) — isolated, low risk.
4. **2c** (params) — mostly falls out of Part 1 + 2a.

Interactions: this composes with [headless-plugin.md](headless-plugin.md) — a
plug-in could be headless *and* paramless *and* meterless. Keep the three
`__has_include` triggers (`editor.hpp`, `params.hpp`/`meters.hpp`, `worker.hpp`)
independent and uniform so combinations don't multiply special cases. None of
this touches the latency protocol or event ordering; the audio-thread
allocation rules are unchanged (everything removed is compile-time gated).
