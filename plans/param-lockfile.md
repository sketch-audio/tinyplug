# Parameter lockfile

Status: **design only, deferred.** Spun out of
[param-identity-and-ordering.md](param-identity-and-ordering.md) step 4. Everything
else in that plan has landed; this is the piece that converts its rules from
convention into enforcement. Held off deliberately — the `au_order()` mechanism
was the urgent part (it had to be in place before any new parameter ships), and
this can follow whenever.

## Why it exists

A parameter model has three permanence surfaces, and the framework can only
check one of them from a single build:

| Surface | Checked today by | Gap |
|---|---|---|
| `identity.address` contiguity | `validate_tree` | can't see *history* — a removed address just shifts everything |
| `identifier` / keypath uniqueness | `validate_tree` | can't see history — a *renamed* identifier still validates |
| `au_order()` completeness | `validate_au_order` | can't see history — a *reordered* list still validates |

`validate_tree` and `validate_au_order` answer "is this model internally
consistent?" Every failure mode that matters is a question about *change over
time*: is this model consistent with the one that shipped? Nothing in the source
can answer that, because "new" is a statement about history. That is what a
checked-in lockfile supplies.

## Shape

A per-plug-in `<Plugin>_paramlock` executable plus a checked-in
`source/models/params.lock` beside `params.hpp`. The tool lives in tinyplug as
`tools/paramlock/` (a `main.cpp` plus `make_paramlock.cmake`), following the
[tools/pagetables/](../tools/pagetables/) pattern — a small C++ manifest binary
driven by a CMake helper that client repos call.

**Do not parse the C++.** The audit behind the parent plan had to regex
`params.hpp` and threw two false positives before it was right. The tool walks
the real `params::Infos<models::Params>` through the same `flatten_tree` /
`au_ordered_copy` / `param_specs` the wrappers use, so it cannot disagree with
what the wrappers do.

**One departure from the parent plan**, which specified linking `<PLUGIN>_lib`:
compile a standalone TU that includes only `models/params.hpp`, tinyplug core,
and the plug-in's generated `plug_info.h`. This still satisfies the rule above —
it links the real model and the real flattening code — but it doesn't drag in the
editor, Skia, and licensing. Verified during the parent migration: a standalone
TU over each model compiles in seconds, where linking `_lib` costs a real build.

Useful side effect: in a debug build, running the tool executes `validate_tree`
and `validate_au_order`, so the lock check subsumes the startup assertions as a
build-time gate.

## The file

Fixed-width text, sorted by AU ordinal, one line per parameter:

```
# tinyplug param lock v1 · doctor_vibe
# regenerate: cmake --build build-debug --target DoctorVibe_relock
#  au  addr  policy      keypath                     semantics
    0     0  automation  vinyl/enabled               bool def=1
    1     1  automation  vinyl/bandwidth             real 0..100 def=100 lin
   50    50  automation  global/mod_seed             int 0..9999 def=0
```

Sorted by **AU ordinal** because that is the surface with the worst failure mode,
and because both it and `address` are append-only — so a legitimately added
parameter is a pure append at the bottom of the file, while an illegal mid-list
insertion appears as an insertion plus a cascade of renumbered lines. The diff
itself tells you which one you did.

Text rather than JSON for the same reason: this file exists to be read in a pull
request.

## What it checks

Three tiers, because the failure modes are not equally bad.

### Hard fail — breaks users who already have the plug-in

- a locked address is absent from the current model (parameter removed)
- a locked address's **keypath** changed (breaks AUv3 host documents and preset recall)
- a locked address's **AU ordinal** changed (remaps Logic automation lanes)
- a new address that is not greater than every locked address (enum not appended)
- a new parameter that is not appended at the end of `au_order()`

### Warn, and make visible in review

- **range, default, or knob adapter** changed for a locked address

The quiet one. A stored automation curve is a normalized value, so widening a
range from `0..100` to `0..200` silently reinterprets every curve a user
recorded. Not a hard fail — deliberate retuning is legitimate — but it should
never happen without someone noticing.

### New finding: `Semantics::List` items are a permanence surface

Not identified in the parent plan. A list parameter's value **is** its index, so
reordering the items, or inserting one anywhere but the end, silently remaps
every stored value and every automation lane. Same class of bug as reordering
`au_order()`, and nothing anywhere currently catches it — `validate_spec` only
checks `def_val < items.size()`.

Record the item strings in the lock and hard-fail on reorder or removal;
append-only like everything else. `doctor_vibe`'s `vin_wow_speed`
(`"33 1/3"`, `"45"`, `"78"`) is exactly the parameter this protects.

## Seeding

Normally the fiddly part, and it isn't here. The lock must record what *shipped*,
not what is on `next` — but the parent migration verified that the current
`au_order()` reproduces the shipped order as an **exact prefix** for all eight
active models against both `498541c` (build 376) and `cf29393` (build 374), and
`da3d51c` for the nuggets. So seeding from the working tree yields the correct
locked prefix by construction, with genuinely-new parameters appended. No
historical reconstruction, no reading old commits.

One intentional exception: `mini_mack`'s order differs from `da3d51c` because
`mode` sits at tree position 2. It has not shipped, so its first release defines
the lock; seeding from HEAD is correct there too.

## Open decisions

Four, none blocking, each with a leaning.

**1. Where it runs.** Build-time custom command on the plug-in target (fails at
your desk), a `ctest` test (`all_plugins` already has `enable_testing()` and a
`tests/` harness with golden fixtures — the natural home), or release-script
only. *Leaning:* wire it as a ctest test **and** call it from
`validate_release.sh`. Build-time would fail fastest but taxes every incremental
build for a check that only matters at commit time.

**2. Which plug-ins get locked.** *Leaning:* opt-in per plug-in via a target
property — on for the eight active models, off for the six still in development.
Locking a model you are actively reshaping means relocking constantly, which
trains exactly the reflex (relock without reading the diff) that defeats the
mechanism. Turn it on at first ship.

**3. Whether to close the policy gap.** Gap 3 in the parent plan is unresolved:
if Logic indexes the *automatable subset* rather than the raw parameter list,
flipping a parameter to `Hidden` shifts everything after it with no reordering at
all. Nobody has tested this. For one extra column — each parameter's ordinal
within the automatable subset — it could be enforced regardless of the answer,
turning an unknown into a checked invariant. Cost: deliberately hiding a shipped
parameter becomes a hard fail requiring an explicit relock. *Leaning:* include
it. Hiding a shipped parameter genuinely might break users, so failing loudly is
the honest default.

**4. The four grandfathered `gain` identifiers.** `dimension_four`,
`swamp_creature`, `trem_bot` and `wobble_head` shipped with an empty `string_id`
on their root `gain` parameter; the migration assigned `"gain"` so the new
non-empty assertion passes, which changes their AUv3 keyPath and preset key from
`""`. None of the four ships factory presets, so exposure is AUv3 host documents
only. If they should instead be frozen as-is, the lock is the mechanism: record
`""` and have `validate_tree`'s non-empty check consult a lock-driven exemption.
This decision belongs here rather than in the framework.

## One caution

`-DTINY_RELOCK=ON` is the escape hatch that disarms all of the above. It must be
noisy — print the exact before/after lines it is about to freeze — so a relock
lands in the commit as a reviewed change rather than a silent file rewrite. A
lockfile that is cheap to regenerate without reading is a lockfile that does
nothing.
