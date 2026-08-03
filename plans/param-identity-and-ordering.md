# Parameter identity, ordering, and permanence

> **Status update — what actually landed (2026-08-03).** Steps 1–3 and the
> `validate_tree` work are implemented in tinyplug and migrated across all 16 client
> models. Two deliberate departures from the proposal below:
>
> - **No `version` / epoch field.** Instead of deriving the AU list by stable-sorting
>   the tree on an epoch number, the model declares the list outright:
>   `au_order() -> std::vector<Address>`, detected by the `params::Au_ordered` concept
>   and consumed only by AUv2 `GetParameterList` via `Param_order::Au_ordinal`. This
>   removes the "default-version trap" (gap 1) entirely — there is no defaulted field to
>   forget, and the order is reviewable as a literal list in the diff. A model that
>   declares no `au_order()` falls back to **address order**, which is append-only by
>   construction since the enum is. Explicitly *not* tree order (the historical
>   behaviour) — falling back to that would leave presentation order load-bearing,
>   which is the constraint this whole design exists to remove.
> - **`Identity` is `{address, identifier}`.** `string_id` is renamed `identifier` on
>   both `Spec` and `Group`, and all spec/group strings moved from `std::string_view`
>   to `std::string` — which also let `loop_garden` drop its interning arena.
>
> Verified: the shipped parameter order is preserved as an exact prefix for all 8
> active models against both baselines (`498541c` / `cf29393`), with only genuinely
> new parameters appended — `glb_mod_seed` for `doctor_vibe`, `output`/`mix` for the
> nuggets. `mini_mack` does not preserve its `da3d51c` prefix (`mode` at tree position
> 2), which is accepted because it has not shipped.
>
> Still open: **the lockfile tooling (step 4)**, now spun out to
> [param-lockfile.md](param-lockfile.md) and deferred, and the Logic experiments
> (step 5). Everything below is the original proposal, kept for its evidence and
> sourcing — note that step 4's format there predates the removal of `version`.

Status: **audit + proposal. No code changed.** Written ahead of tinyplug 1.0, while
the only downstream consumer is [../../all_plugins](../../all_plugins) — 13 plug-ins
(12 shipped) plus 3 nuggets (2 shipped), 16 parameter models in total — and a pivot
is still cheap.

## Verdict up front

We have **four** independent permanence surfaces, not one. Only the first is
documented, and only the first is honoured everywhere.

| # | Surface | Keyed by | Documented? | Honoured today? |
|---|---|---|---|---|
| 1 | Persistence / automation identity | `spec.address` | ✅ CLAUDE.md | ✅ yes |
| 2 | **AUv2 parameter-list order** (Logic automation) | tree preorder | ❌ no | ❌ **1 pending violation** |
| 3 | **AUv3 `keyPath`** | group `string_id` chain + param `string_id` | ❌ no | ⚠️ unvalidated; 4 plug-ins ship empty identifiers |
| 4 | Preset JSON structure | group `string_id` nesting + param `string_id` | ❌ no | ⚠️ unvalidated, silent failure mode |

The good news: across all 16 models the live exposure is **one unshipped parameter**
(`doctor_vibe`'s `glb_mod_seed`); every shipped plug-in and nugget is clean, and the
framework is one small change away from making surface 2 free. The sobering note is
that the *same mistake* appears twice — `doctor_vibe` and `mini_mack` — which is a
base rate rather than a one-off, and the second only escapes because that nugget has
not shipped.

The bad news: we actively tell authors to do the unsafe thing. This line appears in
[template/source/models/params.hpp:18](../template/source/models/params.hpp#L18), in
all four demo models under [examples/](../examples/), and — propagated from the
template — in all 13 shipped plug-ins:

```
// Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
```

Rearranging the tree is exactly what breaks Logic automation. The comment directly
above it (`only add ids, not rearrange or remove`) is correct, which makes the pair
read as a deliberate, considered distinction — "addresses are frozen, the tree is
free". That is the root cause of the one violation we found, and it will keep
producing violations until it is removed from all six sources.

## Evidence: what each surface actually depends on

Established by reading the code, not by inference.

### Surface 1 — identity is `address`, everywhere

Every format derives its parameter identity from `spec.address`:

- **VST3** — `ParameterInfo{.id = static_cast<ParamID>(param.address)}`,
  [controller.cpp:104](../wrappers/vst3/source/controller.cpp#L104)
- **AUv2** — `Globals()->SetParameter(param.address, …)`, and every lookup is
  `params[inParameterID]` against `Param_order::Indexable`
- **AUv3** — `createParameterWithIdentifier:… address:spec.address`,
  [audio_unit.mm:315](../wrappers/auv3/source/extension/audio_unit.mm#L315)
- **AAX** — `tiny_id_to_aax(spec.address)` renders the address as `"0x%08x"`,
  [adapters.hpp:62](../wrappers/aax/source/adapters.hpp#L62). Derived, not authored.
- **CLAP** — `info->id = param.address`

`indexed_specs` is `display_specs` sorted by address, and `validate_tree` asserts
addresses are contiguous `0..num_params-1`. So **`Param_order::Indexable` is exactly
address order**, and it is already append-only for free if the author follows the
documented enum rule.

### Surface 2 — AUv2 list order is tree order

[effect.cpp:229](../wrappers/auv2/source/effect.cpp#L229):

```cpp
const auto& params = User_params::param_specs(Param_order::Presentation);
const auto ids = params | std::views::transform([](const auto& spec) { return spec.address; });
std::ranges::copy(ids, outParameterList);
```

This is the **only** place in the AUv2 wrapper where ordering is used as ordering —
all seven other `Param_order::` sites use `Indexable` purely as an address→spec
lookup table. So the fix, whatever it is, is a one-function change.

**Why it matters** (well-sourced, not inferred):

- clap-wrapper's own extension header, `include/clapwrapper/auv2.h`:
  > "Parameter order matters in auv2 critically still in logic and garage band, and
  > if you add parameters after a release, you need to order them (alas) even if the
  > ids aren't changed."
- baconpaul, directly asked: *"logic pro addresses parameters by id in most cases.
  But logic automation addresses it by index in parameter list yes… and its only
  logic."* Also: *"your param ids need to be stable but also your parameter list
  needs to be append only."*
- Apple is reportedly aware with no plans to fix.

### Surface 3 — AUv3 `keyPath` is the `string_id` chain

Groups: `createGroupWithIdentifier:` from `group.string_id`
([audio_unit.mm:178](../wrappers/auv3/source/extension/audio_unit.mm#L178)).
Params: `createParameterWithIdentifier:` from `spec.string_id`
([audio_unit.mm:300](../wrappers/auv3/source/extension/audio_unit.mm#L300)).
`keyPath` is the dot-joined chain, so **moving a param between groups, or renaming
any ancestor group's `string_id`, changes its keyPath.**

Apple's contract (`AUParameterNode.identifier`):
> "All child nodes, under any given parent, must have a unique identifier. From
> release to release, an audio unit must not change its parameters' identifiers;
> this will invalidate any hosts' documents that refer to the parameters."

Whether Logic actually breaks AUv3 automation on a keyPath change is **untested** —
nobody in the discussion had verified it. Treat the constraint as binding anyway;
Apple states it plainly.

### Surface 4 — presets nest by `string_id`

[state_adapter.cpp:22-40](../libs/tinyplug/source/state_adapter.cpp#L22-L40) writes
`json[spec.string_id]` nested under `group_json[subgroup->string_id]`. A real
shipped preset confirms it:

```json
{ "version": 1, "params": {
    "vinyl":  { "enabled": 0.0, "bandwidth": 100.0, … },
    "global": { "enabled": 1.0, "quality": 1.0, … } } }
```

Presets are **order-independent** — a genuine strength, and it means surface 4 is
immune to the reordering problem that hits surface 2. But it is
*structure*-dependent, and the failure modes are asymmetric and silent
([state_adapter.cpp:97-115](../libs/tinyplug/source/state_adapter.cpp#L97-L115)):

- Missing **param** key → falls back to that param's default. Reasonable.
- Missing **group** key → the entire subtree is skipped, leaving `std::nullopt`,
  and `_update_state` then skips those params entirely, so they **keep whatever
  they currently hold**. A renamed group therefore silently leaves half the preset
  unapplied, with no error anywhere.

### Not permanence-bearing (safe to change freely)

Derived from group **display names**, purely cosmetic: CLAP modules
([adapters.hpp:16](../wrappers/clap/source/adapters.hpp#L16)), VST3 units
([adapters.hpp:61](../wrappers/vst3/source/adapters.hpp#L61)), AUv2 clumps
([adapters.hpp:79](../wrappers/auv2/source/adapters.hpp#L79)).

## Current exposure

Audited all 13 plug-ins against build 374 (`cf29393`, last paid RC) and build 376
(`498541c`, tip of `main`). Identical results for both baselines. 12 of 13 shipped;
`loop_garden` is new.

Reproduce:

```
python3 <scratch>/audit.py cf29393    # or 498541c
```

| Plug-in | params | Result |
|---|---|---|
| dimension_four | 10 → 10 | `gain` + `Gain` group have no `string_id` |
| **doctor_vibe** | **50 → 51** | **`glb_mod_seed` inserted at tree pos 31 → shifts 19 shipped params** |
| fuzz_droid | 25 → 25 | clean |
| galaxy_brain | 88 → 88 | clean |
| gym_rat | 23 → 23 | clean |
| hyper_boost | 29 → 29 | clean |
| instant_ambient | 19 → 19 | clean (the `lpr_*`→`Lpr_*` rename is capitalisation only — same order, same indices) |
| loop_garden | — | new, unshipped |
| scissor_hands | 84 → 84 | clean |
| side_quest | 25 → 25 | clean |
| swamp_creature | 10 → 10 | `gain` + `Gain` group have no `string_id` |
| trem_bot | 10 → 10 | same |
| wobble_head | 10 → 10 | same |

### Nuggets

The three nuggets under [nuggets/](../../all_plugins/nuggets) are a separate release
train. Per Ryan, **only `lil_doc` and `series_9000` have shipped**, and they shipped
*without* the `output` parameter.

Baseline is ambiguous here: the last nugget build commit is `da3d51c` (2026-05-18),
but `main` carries model changes made after it (`series_9000` gained `env_amt`).
Audited against both; the result is the same either way.

| Nugget | shipped → now | added | Result |
|---|---|---|---|
| **lil_doc** *(shipped)* | 5 → 7 | `output`, `mix` | ✅ clean — both appended in enum *and* tree |
| **series_9000** *(shipped)* | 4–5 → 7 | `env_amt`, `output`, `mix` | ✅ clean — all appended in enum *and* tree |
| mini_mack *(not shipped)* | 4 → 7 | `mode`, `output`, `mix` | ⚠️ `mode` at tree pos 2 shifts 2 — harmless only because it never shipped |

Structurally the nuggets are the easy case: **completely flat, no groups**, so
keypaths are single segments with no ancestry, presets are flat JSON, and surfaces 3
and 4 have nothing to break. All identifiers are present and unique. All three build
all five formats.

Two things worth carrying forward:

- **`mini_mack` is the second instance of the exact same mistake.** Like
  `doctor_vibe`, its enum was appended perfectly (`mode` is at enum index 4, after
  `edge`) while the tree placed it at position 2. Two occurrences across 16 models is
  a base rate, not an accident — it is what the author's instinct produces when the
  docs say the tree is free. `mode` was commented out at `da3d51c` and uncommented
  later, so it was a deliberate staged addition that still landed in the unsafe spot.
- **Epoch assignment for the nuggets:** `lil_doc` and `series_9000` get `version = 0`
  for their shipped parameters and `version = 1` for `output`/`mix`. `mini_mack` gets
  `version = 0` throughout — nothing has shipped, so its *first* release defines
  epoch 0 and `mode` may stay where it is in the tree. For `series_9000`'s `env_amt`,
  confirm whether it shipped; either epoch is safe because it is appended in both the
  enum and the tree, but the lock should be seeded from the binary that actually went
  out.

### The one real violation

`doctor_vibe`'s `glb_mod_seed` (commit `dca2f8a`, "Doctor Vibe gets Repeatable
Random") did **everything the documentation asks**:

- appended to the `Address` enum at the end (index 50) ✅
- given a `string_id` ✅
- placed in the tree next to its siblings, inside `global` between `auto_bypass`
  and `a_settings` — which the template comment explicitly permits

…and still shifts 19 already-shipped parameters in the AUv2 list. This is the
clearest possible demonstration that the invariant cannot be left to convention.

**It has not shipped.** `git show main:…params.hpp | grep glb_mod_seed` → 0 hits.
It exists only on `next`, after build 376. So this is a *pending* regression we can
still fix for free, not a live one we have to migrate around.

### Secondary finding: empty AUv3 identifiers

`dimension_four`, `swamp_creature`, `trem_bot`, `wobble_head` each declare a root
`gain` param and a `Gain` group with **no `string_id`**, producing an empty
`AUParameter.identifier` — which contradicts Apple's uniqueness requirement and
would collide if a second unnamed sibling were ever added. Also means their preset
JSON would use `""` as a key. These four **have shipped**, so any fix here is a
migration, not a free change. None of the four ships factory presets, which limits
the blast radius to AUv3 host documents.

## The "one permanent thing" question

Your stated ideal: the author commits to the **address** and everything else is
derived. Here's how close each surface can get.

| Surface | Derivable from `address` alone? | Cost |
|---|---|---|
| 1. Identity | already is | — |
| 2. AUv2 order | **yes** — sort by address | loses author control of AU display order |
| 3. AUv3 keyPath | yes, e.g. `"p42"` | unreadable; **breaks 12 shipped plug-ins** |
| 4. Preset keys | yes, key by address | unreadable, un-diffable; **breaks all shipped presets** |

So surfaces 3 and 4 *could* be derived, but both are one-way doors we've already
walked through — 12 plug-ins shipped with `string_id`-based keyPaths and preset
files. Deriving them now would invalidate existing AUv3 host documents and every
factory/user preset on disk.

**Realistic target: two permanent things.** The `address`, and the `string_id`
(param-local, plus its group chain). That's still a big simplification over today's
*four* implicit contracts, and it's achievable without breaking anything shipped.

## The design

**The tree keeps encoding permanence.** Authoring full keypaths on every parameter
would state the hierarchy twice — once in the tree nesting, once in the path
strings — and the two could disagree. Deriving is fine *provided it is enforced*,
and enforcement is a build-time concern, not a syntax concern.

### Shape

```cpp
// Everything in here is frozen the moment a plug-in ships.
struct Identity {
    uint32_t address{};              // persistence key; enum_raw(Address::X)
    uint32_t version{};              // the epoch that introduced this parameter
    std::string_view identifier{};   // leaf name; keyPath = ancestry + this
};

struct Spec {
    Identity identity{};
    std::string_view name{""};       // free to change
    std::string_view short_name{""}; // free to change
    Semantics::Any semantics{...};
    Policy policy{...};
};

struct Group {
    std::string_view name{""};       // display only, free to change
    std::string_view identifier{};   // frozen — part of every descendant's keyPath
    std::vector<Node> nodes{};
};
```

`string_id` is renamed `identifier` on both, matching Apple's vocabulary
(`AUParameterNode.identifier`) and making the AUv3 contract legible at the
declaration site.

### AU ordering

```cpp
// Epoch-major, tree order preserved within each epoch.
inline static const std::vector<Spec> au_specs =
    impl::stable_sorted_copy(display_specs,
        [](const auto& a, const auto& b) { return a.identity.version < b.identity.version; });
```

`std::stable_sort` on `version` alone is the entire mechanism.

## Sanity check: does this actually resolve it?

**Yes, conditional on rule 4 below.** The argument, then the measurement.

### The argument

The AU list is `concat over v of [params with version v, in tree order]`.

For the list to be append-only across a release, the prefix of previously-shipped
parameters must be unchanged. Every new parameter carries a strictly greater
version, so all new parameters sort *after* every shipped one — the prefix can only
be disturbed if the **relative tree order of same-version parameters changes**.
Inserting a node never reorders its siblings, so insertion is always safe. Moving an
existing parameter is the only operation that breaks it, and rule 4 forbids it.

### The measurement

Simulated over the real models: assign `version = 0` to everything present at build
376, `version = 1` to everything added since, then `stable_sort` the current tree by
version and compare the prefix against the shipped order.

| Plug-in | shipped → now | shipped order preserved as prefix |
|---|---|---|
| dimension_four | 10 → 10 | ✅ |
| **doctor_vibe** | **50 → 51** | ✅ — `glb_mod_seed` appended at ordinal 50 |
| fuzz_droid | 25 → 25 | ✅ |
| galaxy_brain | 88 → 88 | ✅ |
| gym_rat | 23 → 23 | ✅ |
| hyper_boost | 29 → 29 | ✅ |
| instant_ambient | 19 → 19 | ✅ |
| scissor_hands | 84 → 84 | ✅ |
| side_quest | 25 → 25 | ✅ |
| swamp_creature, trem_bot, wobble_head | 10 → 10 | ✅ |

**All 12 shipped orders preserved exactly**, and `doctor_vibe`'s new parameter
stays visually inside the `global` group while landing last in the AU list. That is
the whole point: the author's natural instinct stops being dangerous.

Compare with the alternative of sorting by `(version, address)` — the Six Sines
scheme — which would reorder 4 of the 12 at adoption (`fuzz_droid` 4 params,
`doctor_vibe` 18, `galaxy_brain` 19, `scissor_hands` 29, max shift 9). Sorting by
tree order within the epoch is what makes adoption free.

### Where it does not save us

Four honest gaps.

1. **The default-version trap — the sharpest edge.** If `version` defaults to `0`
   and an author forgets to bump it, the new parameter sorts into epoch 0 *at its
   tree position* and inserts mid-list. This is exactly the `doctor_vibe` failure,
   reintroduced. Nothing in the source can catch it, because "new" is a statement
   about history. The lockfile check is therefore **not optional**: any address
   absent from the lock must carry `version > max(version in lock)`.
2. **Epoch monotonicity.** A release that reuses or lowers an epoch number sorts its
   parameters before an existing epoch and breaks it. Same lockfile rule covers this.
3. **`Policy` changes are unaddressed.** If Logic builds its automation list from the
   *filtered* set of automatable parameters rather than the raw list, then flipping a
   parameter from `Automation` to `Hidden` shifts everything after it without any
   reordering at all. Unknown, and this design does not defend against it. Record
   `policy` in the lockfile so at minimum the change is visible in review.
4. **AUv3 is assumed order-insensitive.** The design leaves the AUv3 tree alone,
   because Apple documents identity via `keyPath`. If the experiment below shows
   Logic uses ordinals for AUv3 too, the epoch sort must also apply *within each
   group* of the AUv3 tree — and groups would then need versions of their own. That
   is a materially larger change; flagged as a contingency, not scoped here.

## The four rules

1. **`identity.address`** — assign at the end of the enum. Never change, reuse, or
   remove. (Retire a parameter by setting `Policy::Hidden`; it keeps its slot.)
2. **`identity.version`** — set to the current epoch when adding a parameter. Never
   change afterwards. Each new epoch must exceed every existing one.
3. **`identity.identifier`** and group `identifier` — never change. A parameter
   **may not move between groups**, because its keyPath is its ancestry plus its own
   identifier.
4. **Tree order** — insert new parameters anywhere. Never reorder parameters that
   share a `version`.

Free at all times: `name`, `short_name`, group `name`, and the entire display
hierarchy *provided ancestry is preserved*.

## Changes required

### 1. `libs/tinyplug/include/tinyplug/tiny_params.hpp`

- Add `struct Identity`; fold `address` and `string_id` into it on `Spec`.
- Rename `Group::string_id` → `Group::identifier`.
- Add `Param_order::Au_ordinal`; add `au_specs` (stable sort of `display_specs` by
  `identity.version`); return it from `param_specs`.
- `indexed_specs` sorts by `identity.address` — unchanged behaviour.
- Extend `validate_tree`, which today only checks address contiguity:
  - every `identifier` non-empty
  - `identifier` unique among siblings (the header already *claims* this)
  - derived keyPaths globally unique

### 2. Wrappers

| File | Change | Kind |
|---|---|---|
| [auv2/effect.cpp:229](../wrappers/auv2/source/effect.cpp#L229) | `Param_order::Presentation` → `Au_ordinal` | **the fix** |
| [auv3/audio_unit.mm:300](../wrappers/auv3/source/extension/audio_unit.mm#L300), [:178](../wrappers/auv3/source/extension/audio_unit.mm#L178) | `spec.string_id` → `spec.identity.identifier`; `group.string_id` → `group.identifier` | mechanical |
| [vst3/controller.cpp:104](../wrappers/vst3/source/controller.cpp#L104) | `param.address` → `param.identity.address` | mechanical |
| [clap/plugin.cpp](../wrappers/clap/source/plugin.cpp) | `param.address` → `param.identity.address` | mechanical |
| [aax/adapters.hpp:127](../wrappers/aax/source/adapters.hpp#L127) | `spec.address` → `spec.identity.address` | mechanical |
| [libs/tinyplug/source/state_adapter.cpp](../libs/tinyplug/source/state_adapter.cpp) | `spec.string_id` → `spec.identity.identifier` | mechanical |

Exactly **one** behavioural change in the whole framework — the AUv2 line. Every
other edit is a field rename. Note that all seven other `Param_order::` sites in the
AUv2 wrapper use `Indexable` purely as an address→spec lookup and are unaffected.

### 3. Lockfile tooling (new)

A `<Plugin>_lockcheck` executable that links `<PLUGIN>_lib`, walks `param_tree()`
through the framework's own flattening code, and diffs against a checked-in
`source/models/params.lock`. **Do not parse the C++** — the audit that produced this
document had to regex `params.hpp` and threw two false positives before it was
right. Linking the real model cannot disagree with what the wrappers do.

```
# tinyplug parameter lock — regenerate with -DTINY_RELOCK=ON
# au_ordinal  address  version  policy      keypath
   0            0        0      automation  vinyl/enabled
   …
  50           50        1      automation  global/mod_seed
```

Failure conditions: keypath changed, au_ordinal changed, parameter removed, new
address not appended, version changed for an existing address, new address with
`version <= max(locked versions)`. Also record range/default — changing a range
silently changes what every stored automation curve *means*.

### 4. Docs and template

The template comment is the root cause of the one violation found and must go. It
appears in **six** places: [template/source/models/params.hpp:18](../template/source/models/params.hpp#L18)
and the four demo models under [examples/](../examples/), and is propagated into all
13 client models.

```diff
- // Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
+ // Once you ship a plug-in the tree is a permanence surface, not just presentation:
+ //  - never reorder parameters that share a `version` (breaks Logic AUv2 automation)
+ //  - never move a parameter between groups, and never change an `identifier`
+ //    (breaks AUv3 host documents and preset recall)
+ // New parameters: append the address, bump `version`, place them wherever you like.
```

Add the four rules to CLAUDE.md, and update
[tools/new_plugin.py](../tools/new_plugin.py) if it templates the model.

### 5. Client plug-ins (`../../all_plugins`)

Smaller than it looks. Grepped `plugins/` for `.address` / `.string_id` outside the
model files: the only hits are `Led_lamp::Spec` / widget specs in `editor.cpp` and
`meters::Spec` in `peak_label.cpp` — different types, unaffected. Everything else
addresses parameters through `enum_raw(Address::X)`, which does not change.

So the migration is **16 files** — 13 under `plugins/`, 3 under `nuggets/` —
mechanically:

```diff
  Spec{
-     .address = enum_raw(Glb_mod_seed),
-     .string_id = "mod_seed",
+     .identity = {.address = enum_raw(Glb_mod_seed), .version = 1, .identifier = "mod_seed"},
      .name = "Seed",
```

- **Plug-ins:** every parameter present at build 376 gets `.version = 0`.
  `doctor_vibe`'s `glb_mod_seed` gets `.version = 1` — this alone fixes the pending
  regression, with the parameter staying where it belongs in the UI. Seed each
  `params.lock` from the **build 376** tree order, not from `next`, so the lock
  records what actually shipped.
- **Nuggets:** `lil_doc` and `series_9000` get `.version = 0` for shipped parameters
  and `.version = 1` for `output`/`mix`; seed their locks from the shipped nugget
  build. `mini_mack` gets `.version = 0` throughout and its lock seeded from
  whatever ships *first*, since nothing has gone out — `mode` may stay at tree
  position 2.
- Note the nuggets are still on the pre-refactor `param_model.h` / `Param_address`
  naming on `main`; the working tree has already moved them to
  `params.hpp` / `Address`, so the migration rides along with that rename.
- The four plug-ins with an empty `identifier` on `gain` (`dimension_four`,
  `swamp_creature`, `trem_bot`, `wobble_head`) are grandfathered: freeze the empty
  value in the lock rather than "fixing" it, since assigning one now would change
  their keyPaths. Add the `validate_tree` non-empty check as a warning with a
  lock-driven exemption, or accept the break — none of the four ships presets, so
  the exposure is AUv3 host documents only.

### Suggested order

1. Template/CLAUDE.md comment fix — five minutes, stops the bleeding.
2. `Identity` + `Au_ordinal` + `validate_tree` in the framework; mechanical wrapper renames.
3. Client migration (13 files) + `doctor_vibe` `version = 1`.
4. Lockfile tooling, seeded from 376.
5. The Logic experiments below.

Steps 1–3 are safe to land independently; step 4 is what converts the rules from
convention into enforcement, and step 5 may reopen the AUv3 contingency.

## What still needs verifying empirically

Honest boundaries. None of these block the design, but two could change its scope.

1. **The Logic AUv2 failure mode is sourced but unobserved by us.** baconpaul
   confirmed the mechanism directly and clap-wrapper's header documents it, but we
   have not seen what breakage looks like — silent remapping, a dropped lane, or
   something milder. Build a demo AUv2, automate two parameters in Logic, save;
   insert a parameter mid-tree, rebuild, reopen, inspect the lanes. ~1 hour, and it
   calibrates how hard the lock check should fail.
2. **AUv3 keyPath breakage in Logic is untested by anyone** in the discussion that
   prompted this. Same experiment shape. If Logic turns out to use ordinals for AUv3
   as well, gap 4 above becomes live and groups need versions.
3. **Does Logic index the raw parameter list or the automatable subset?** Determines
   whether `Policy` changes are a fifth permanence surface (gap 3 above).
4. **AAX**: parameters are addressed by string IDs derived from the address, so
   registration order should be identity-neutral. Unconfirmed against Pro Tools
   automation lanes; low risk.
