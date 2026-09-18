# Bounce Parameter Authority — deferred

_Sessions 2026-09-16 and 2026-09-18. **Deferred by Ryan 2026-09-18.** Both repos returned to
a clean state; nothing from this work is in the tree. This document is the whole record —
it carries the diagnosis, the measured fix, the exact diff to re-apply, and the risk list
that has to be worked through before it ships._

Lives in `tinyplug` because the fix is wrapper-side. `all_plugins` needs **no changes at all**.

---

## Status

| | |
|---|---|
| Diagnosis | **Complete.** Root cause understood and measured. |
| Fix | **Written and verified twice** (AUv2/Logic), then reverted. |
| API impact | **None.** `Reset::Hard`/`Soft` preserved. |
| Blocking issue | Risk #1 below — the deferral also fires on realtime transport, where it is audible. |
| Tree state | Clean. Autolog instrumentation deleted (see "Re-adding the instrumentation"). |

**To resume:** apply the diff in "The fix", rebuild with `-DTINY_AUTOLOG=1`, work the risk
list in "Recommended order" — starting by gating the deferral on `Render_mode::Offline`.

---

## TL;DR

Two bounces of the same Logic project produce different audio. The cause is **not** our
automation handling — the host's automation delivery is byte-identical between takes. The
plug-in renders the first block of every render with **whatever parameter values the last
playhead locate left behind**, because neither the host's store nor our own ramper targets
have been updated for the render-start position yet.

The damage is permanent only for state that integrates without decay (Galaxy Brain's
frequency-shifter carrier phase). Everything else forgets within milliseconds, which is why
this shipped.

**Solved for AUv2/Logic, verified twice, in two different shapes.** The final shape needs no
API change: at the first block reaching the timeline, restate every parameter from the
host's store as ordinary `Event::Set`s, then issue the existing `Reset::Hard{}`.

---

## The defect, precisely

1. At the start of an offline render, the plug-in's parameters **and** Logic's own
   `Globals()` store hold the automation value at the **playhead** — not at the render-start
   position. The two agree with each other, so no host<->plug-in resync can detect it. The
   staleness is internal to Logic, between its live parameter state and its own lane.

2. Logic implements sample-accurate plug-in automation by **subdividing the render buffer**
   (multiples of 32 samples, max 384; 1024 when no automation is active) and calling
   `SetParameter` at offset 0 between sub-blocks. It never calls `ScheduleParameter`. It
   delivers from its first 32-sample grid boundary onward — the render's initial alignment
   remainder gets nothing. **This is specific to a render starting at the timeline origin**:
   a midstream render's first block arrives carrying events (see Verification).

3. That first block is therefore rendered on the playhead's value. Its input and output are
   **bit-exact silence**, but it still advances effect state.

4. The playhead is usually parked somewhere different before each take, so the stale value
   differs — which is what makes the renders differ. Park the playhead identically and two
   bounces match, while both remain wrong relative to a proper chase.

### What the committed code actually depends on

Worth stating precisely, because it is easy to get wrong. The committed path at a bounce
edge is:

```cpp
if (render_mode_changed) {
    _processor->reset(process::Reset::Hard{});   // = clear(); snap();
```

`snap()` settles the adapter's **own held ramper targets**. `Globals()` is never consulted.
Those targets are nothing but the accumulated residue of every `SetParameter` delivered so
far — i.e. the value at wherever the playhead last sat. So the dependency is **not** "the
host synchronised its store". It is:

> The host must have **delivered** the render-start values as parameter events **before the
> first offline `Render` call**.

The window closes at the first offline block, not at the first event: `Hard{}` fires at
`effect.cpp:1095` while the event drain (`_processor->handle`) is at 1255/1282. An event at
offset 0 of the bounce's own first block is already too late — `snap()` has settled the
stale target, and the correct value arrives afterwards as a new target the ramper **glides
into from stale**.

This is exactly why Option 1 (read `Globals()` at the render-mode edge) measured no
improvement: at that edge in Logic both the store *and* our targets are stale.

### Hosts where the committed code is already correct

- **Ableton — measured.** Two bounces bit-identical under committed code: render starts at
  pos 0, no pre-roll, store already holds the render-start value at the reset. It chases
  *before* the render begins, so delivery lands inside the window.
- **Logic, midstream renders — inferred.** The bar 5-9 run showed block 0 carrying 4 events
  with `GLOBALS addr 1 = -1.99999809` correct and identical in both takes, so the chase was
  delivered before that block. Committed code should reproduce there for the same reason.
  *Not directly measured* — the midstream run was done with the fix applied.

So the shipped defect is narrow: **Logic, on a render that starts at the timeline origin** —
the one case where Logic behaves like a cycle wrap rather than a locate and delivers no
chase at all.

### Why it survived to ship

On most effects it is a sub-20 ms transient at the head of a file. And reproducibility
testing that bounces twice without touching the transport **passes**, because bounce 2 and
bounce 3 both start from bounce 1's end value. Only bounce 1 differs. (Predicted, not yet
verified — see "Open questions".)

---

## Evidence

Measured with `TINY_AUTOLOG` instrumentation (see "Re-adding the instrumentation").

- **Automation delivery is identical between takes.** Offline-only APPLY streams: 1357
  events (Galaxy Brain) / 1693 (Scissor Hands), same values at the same absolute timeline
  positions, both bounces. The early "delivery race" hypothesis is **dead**.
- **`GLOBALS` differs in exactly one parameter — the automated one.**
- **Block 0 is silence in, silence out.** Its `ohash` matches a synthetic all-zeros FNV to
  the bit (2048 / 2304 zero bytes). It primes nothing and cannot.
- **Logic has a chase and skips it here.** In realtime, pressing play delivers **88 events
  in the first block** — every parameter, `addr 87->0`, at `pos=-428`. A **cycle wrap**
  delivers **0**. An **offline bounce start** delivers **0**. So a bounce behaves like a
  wrap, not like a locate. Logic isn't missing the machinery; a bounce start isn't a locate.
- **Looped playback has the same gap** — every lap begins with ~428 samples on the previous
  lap's final value.
- **Cycle on vs cycle off made no difference.** Logic reports `cyc=0` during an offline
  render either way, same block layout.

### Host comparison (same plug-in, same wrapper)

| | Logic | Ableton |
|---|---|---|
| Render start | pos **-284** (pre-roll) | pos **0** — none |
| Block sizes | subdivided 32...384, 1024 idle | fixed **2048**, never split |
| Automation offsets | always `off=0` | **real offsets** — 4, 260, 332, 388, 428, 908... |
| First block | `events=0` | **`events=1`** |
| Stale window | **288 samples** | **4 samples** (= reported latency) |
| `Globals()` at the reset | playhead value — **stale** | **correct render-start value** |
| Two bounces | diverged | bit-identical |

---

## The three layers

Contamination from the stale-parameter window lands in three distinct places. Any fix has
to be evaluated against all three.

| # | Layer | Fix | Recovers on its own? |
|---|---|---|---|
| 1 | Adapter's parameter ramper glides from the stale value | land the first post-discontinuity value | **yes, exactly, in finite time** — see correction below |
| 2 | The **effect's own internal smoothers** (fed by `prepare()`, glide regardless of input) | `snap()` reaches them | decays — Scissor Hands' 14.7 ms residual |
| 3 | **Non-decaying state** — Galaxy Brain's carrier phase | must not advance on unauthoritative params | **never** |

> **Correction (2026-09-18).** An earlier revision of this plan said layer 1 "never exactly
> converges, with `One_pole`". That is wrong for the shipping catalogue. Every product
> delegates to `default_ramp_policy` (`shared/processor/effect_shared.hpp`), which returns
> `Linear` for `Real`/`Fixed` and `Instant` for everything else. **No product selects
> `One_pole`** — the overrides in `galaxy_brain` and `scissor_hands` go the *other* way,
> toward `Instant`, so modulation rates don't smear phase. Layer 1 therefore converges
> exactly after the ramp duration. The permanent damage is layer 3 alone.

Layer 3 needs no dedicated fix. `clear()` sets `_moving = false`, so a deferred discontinuity
makes the next `process()` see the `!_moving && moving` edge again and re-fire
`timeline_started()` -> `_shifter.restart()`. The carrier re-anchors *after* the parameters
are authoritative instead of before the stale pre-roll. (This re-fire is also risk #2 — see
Risks.)

Layer 2 was the surprise — pre-roll is silence, so no filter *history* is excited. The
residual is **coefficient** history.

> **The invariant worth keeping:** *no state that integrates without decay may advance on
> unauthoritative parameters.*

### Practical severity, by product

- **Layers 1-2 only:** a head transient that dies within tens of milliseconds; the rest of
  the file is bit-identical. Two bounces differ only at the very start.
- **Layer 3 present (Galaxy Brain; Doctor Vibe, cause unidentified):** the whole render is
  offset. Permanent. This is the real bug.

Discrete params (`Bool`/`List`/`Int`) are `Instant`, so a stale mode doesn't glide — but
whatever the DSP did while holding the wrong mode can still feed layer 3.

---

## Options tried, and why they were rejected

**Option 1 — restate from `Globals()` at the render-mode edge.** Implemented, measured, no
improvement. `Globals()` is stale in exactly the same way in Logic; it swapped one stale
source for another. *Still useful under Ableton*, where the store is correct.

**Option 2 — snap-on-first-value latch in the client.** A per-parameter "next `set` lands"
flag armed by `clear()`. Worked: Scissor Hands 1.115 s -> **14.7 ms**. Rejected on design
grounds — Ryan wanted the client to blindly follow directions, not carry hidden state.

**Option 3 — defer only the `snap()`.** Rejected: `clear()` at block 0 doesn't prevent
pre-roll contamination (the damage happens *after* the clear), and `Ramped_params::clear()`
was a **no-op**, so clear/snap didn't partition parameter state the way the names suggest.

**Option 4 — defer the whole discontinuity until the first block reaching the timeline.**
**The mechanism that works.** Bit-identical on both instrumented products, twice.

**Option 5 — a reset that carries the values.** Correct and necessary: deferral alone is not
enough (see below). First expressed as a `Clear`/`Sync{params}` API rewrite, then — in the
final shape — as a plain restatement through `handle()`, which needs no API change at all.

**Option 6 — wrapper applies offset-0 events, then resets.** Rejected: relies on call
ordering, which tinyplug's own AAX source warns against — *"an unenforceable rule with a
silent, format-specific failure mode."* Also only covers parameters whose first event is at
offset 0, and the equal-offset sort is `std::ranges::sort` (not stable).

**Option 7 — one-block lookahead** (buffer pre-roll, re-run once values land). Correct but
costs latency/PDC churn and scales badly. Not pursued.

**Option 8 — PLL / position-derived carrier phase.** Rejected for audio-rate oscillators:
slewing carrier phase *is* frequency modulation and would be audible. The requirement is
only that phase be a deterministic function of the timeline — **anchoring** achieves that
exactly, with no approximation.

### Why both halves are load-bearing

- **Deferral alone is not enough.** `_set_param` sets ramper *targets*; `snap()` settles
  them. A deferred `Hard{}` with no restatement settles the *stale playhead* targets, and
  the block's own event then sets a new target the ramper glides into from stale.
- **Restatement alone is not enough.** That is Option 1: at the pre-roll block Logic's store
  is stale too, so there is nothing good to read.

---

## API decision: `Hard`/`Soft` stays

An intermediate version renamed `Reset::Hard`/`Soft` to `Clear`/`Sync{std::span params}` so
the reset could carry the host's values. It worked, but it cost a migration across **26 call
sites** — vst3 (4), clap (6), auv3 (4), aax (1), 2 examples, and 8 files in
`all_plugins/tests/`, several of which deliberately exercise the Hard-vs-Soft distinction
and need thought rather than a rename.

**That rewrite is unnecessary.** The wrapper can restate the values as ordinary
`Event::Set`s through `handle()` before issuing the existing `Reset::Hard{}`. Measured
identical (see Verification). Keep `Hard`/`Soft`.

Two things that make this safe, both verified in code:

- `_request_quality` is **edge-guarded** (`if (quality == _target_quality.value_or(_quality))
  return;`), so restating a quality value the adapter already holds schedules nothing.
- `clear()` touches none of `_param_manager`, `_pending_gains`, `_abp_ctrl.state` or
  `_param_change_flush_ctr`, so reordering to `restate; clear; snap` is inert versus
  `clear; restate; snap`.

If the API is ever revisited anyway, `struct Hard { std::span<const double> params{}; }` is
strictly additive — `Reset::Hard{}` keeps compiling and behaving identically everywhere —
and it restores the `_target_quality` fold for free (risk #3).

---

## The fix

Entirely within `wrappers/auv2/source/effect.{hpp,cpp}`. Shown stripped of the autolog
instrumentation that surrounded it.

### `effect.hpp` — private section

```cpp
// Restate every parameter from the host's store straight into the processor, ahead
// of the `Reset::Hard` that settles them. Render thread only.
auto _restate_params_from_globals(const char* reason) -> void;

// A render that opens with pre-roll gets no automation for it, and the host's store is
// still the last locate's, so neither half of a discontinuity can be resolved there.
// Both wait for the first block that reaches the timeline proper. Render thread only.
bool _pending_reset{};
int _pending_reset_blocks{};
const char* _pending_reset_reason{"?"};
```

### `effect.cpp` — the two discontinuity sites only raise a flag

```cpp
// was: _processor->reset(process::Reset::Hard{});
if (_needs_clear.exchange(false, std::memory_order_relaxed)) {
    _pending_reset = true; _pending_reset_blocks = 0; _pending_reset_reason = "host reset";
    _bypass.clear();    // Its delay lines hold pre-seek dry audio.
    _bypass.snap();
}
```

```cpp
// was: _processor->reset(process::Reset::Hard{});
if (render_mode_changed) {
    _pending_reset = true; _pending_reset_blocks = 0; _pending_reset_reason = "render-mode change";
    _bypass.clear();    // Its delay lines hold pre-bounce dry audio.
    _bypass.snap();
}
```

### `effect.cpp` — the restatement

```cpp
// Restate every parameter from the host's store, as the events the processor already
// understands. Whether the store is authoritative at this moment is the host's business:
// Ableton resyncs it to the render start before the bounce edge, Logic leaves it holding
// the value at the playhead — which is why the caller defers this to a block where Logic
// has spoken. Same restatement the resync branch in `Render` performs, applied directly
// rather than queued, because it has to land before the reset that settles it.
auto Effect::_restate_params_from_globals(const char* reason) -> void
{
    using namespace params;

    const auto& specs = User_params::param_specs(Param_order::Indexable);

    for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
        const auto host_value = Globals()->GetParameter(static_cast<AudioUnitParameterID>(addr));
        const auto plain_value = Value_helper::host_to_plain(host_value, specs[addr].semantics);

        _processor->handle(process::Event::Set{.address = static_cast<uint32_t>(addr), .value = plain_value});
    }
}
```

### `effect.cpp` — resolution, in `Render`, after `host_data` is gathered and **before** the event drain

```cpp
const auto base_pos = static_cast<int64_t>(std::llround(host_data.sample_pos));

// Resolve the discontinuity here rather than where it was raised. A render that opens
// with pre-roll is handed no automation for it and the host's store still holds the
// last locate's values, so a reset there would clear to a state nobody has described.
// The first block reaching the timeline proper is the first at which the host has
// spoken. Clearing late also discards what pre-roll advanced — which is the point: it is
// silence in and silence out, and what it advanced was advanced on stale values.
// Bounded, so a render that never reaches the timeline still resolves.
if (_pending_reset) {
    const auto reaches_timeline = (base_pos + static_cast<int64_t>(nFrames)) > 0;
    if (reaches_timeline || ++_pending_reset_blocks >= 16) {
        // Order is load-bearing. `Hard` is `clear(); snap();` and `snap` settles
        // whatever targets are held, so the restatement has to set them first —
        // reversed, `snap` lands the stale playhead values and the restated ones are
        // left to glide in from them, which is the divergence this exists to close.
        _restate_params_from_globals(_pending_reset_reason);
        _processor->reset(process::Reset::Hard{});
        _pending_reset = false;
    }
}
```

No latency adjustment is needed. The trigger uses the host-reported position, which already
carries the plug-in's PDC shift (Galaxy Brain `lat=4` reports `-284` where Scissor Hands
`lat=0` reports `-288`). Both land on block 1, before any exported audio and after the
host's first automation delivery.

**Behaviour change to keep in mind:** deferring means block 0 is no longer cleared, so it
renders with pre-bounce state and its output is no longer silence. In an offline render it
is pre-roll and never reaches the file, and block 1's clear wipes it. **In realtime it is
heard** — see risk #1.

---

## Verification

### 2026-09-16 — `Clear`/`Sync{params}` shape (API rewrite, since abandoned)

| Test | Product | Result |
|---|---|---|
| SINE | Galaxy Brain | **BIT-IDENTICAL** |
| SAW | Scissor Hands | **BIT-IDENTICAL** |
| NOISE | Doctor Vibe — *uninstrumented control* | differs |

```
phash over OFFLINE blocks:  1/2277 differ (Galaxy Brain)   — block 0 only
                            1/2598 differ (Scissor Hands)  — block 0 only
```

**Midstream bounce (bars 5-9, cycle range) — also bit-identical**, with no divergence at all,
not even block 0. Logic chases for a midstream render: block 0 is a 32-sample sliver at
pos 383716 arriving with 4 events, and `GLOBALS addr 1 = -1.99999809` in both takes. So
Logic's failure to chase is **specific to a render starting at the timeline origin**.

### 2026-09-18 — `restate; Hard{}` shape (the one in "The fix")

Region-start bounce, playhead parked differently before each take.

| File | Product | WAV `data` chunk |
|---|---|---|
| SINE | Galaxy Brain | **IDENTICAL** `4009b756ae91` |
| SAW | Scissor Hands | **IDENTICAL** `473f0e7b0d86` |
| NOISE | Doctor Vibe — *control* | differs — `0c6775006fde` vs `c5fea432b817` |

> Compare the **`data` chunk**, not the whole file. Logic stamps `bext`, `ResU` and `LGWV`
> (waveform overview cache) per bounce, so whole-file hashes always differ.

```
phash / ohash over OFFLINE blocks:  1/2117 differ (Galaxy Brain)   — block 0 only, pos -284
                                    1/2438 differ (Scissor Hands)  — block 0 only, pos -288
```

A hard test: the stale values were far apart — `addr 1: 11.4599943 vs -25.4599988` and
`addr 11: 812.513173 vs 5482.17358` (a 6.7x cutoff gap). APPLY streams identical
(1357 / 1693 events), so delivery was the same and the divergence was purely the
stale-value path.

**The two shapes produce identical internal state.** Block 1's canonical `phash` is
`f50ba0ca4437557b` in *both* the Clear/Sync run and the restate/Hard run — so the
`restate; clear; snap` reordering is confirmed inert by measurement, not by argument.

The mechanism, from the log:

```
BLK 0 pos=-284 events=0
  ADPT timeline_started spos=35412     <- old transport state, pre-clear
BLK 1 pos=-28 events=1
  RESTATE+HARD (render-mode change)    <- restatement fires
  ADPT reset Hard spos=-28
  APPLY SET addr=1 val=0 abs=-28       <- host's own event lands after; already 0, no-op
  ADPT timeline_started spos=0         <- re-fires -> shifter re-anchors (layer 3)
  phash=f50ba0ca4437557b
```

---

## Risks and unintended side effects

Worked through 2026-09-18 against the code, ahead of propagating to the other wrappers.
**This list is why the work was deferred.** Numbered by severity.

### High

**1. The deferral is not bounce-only — it fires on realtime transport, where it is audible.**

`_needs_clear` is set by `Effect::Reset()` (`effect.cpp:141`), which hosts call on transport
stop/start/seek and un-bypass — not only at bounce edges. Logic's realtime play from the
song start reports `pos=-428`, so pressing play at bar 1 takes the deferral path, and
realtime output is **heard**; there is no pre-roll to discard.

Worse, `_bypass.clear()/snap()` still runs *immediately* at the edge while the processor's
clear waits. For that block the dry path is cleared and the wet path is not, so cleared dry
mixes with a fragment of the previous position's delay line — roughly 6.5 ms at 44.1k.

*Mitigation:* gate the deferral on `render_mode == Offline`, or apply it only to the
`render_mode_changed` trigger and leave `_needs_clear` immediate. **Do this first.**

**2. `timeline_started()` now fires twice per bounce, the first time on a stale position.**

Visible in the log above: `spos=35412`, then `spos=0`. Committed code fires it once. The fix
*depends* on the second fire — that is the layer-3 mechanism — but any `Principal_effect`
whose `timeline_started()` is not idempotent now runs it twice, once on garbage. One-shots,
counters and parity flips are exposed; that is the shape of the `Crackle::Pop` flip bug.
Propagating across 4 wrappers x every product multiplies this, and **no wrapper-level test
will catch it** — it is per-product.

**3. The `_adopt_params` latency guard was dropped.**

`_adopt_params` force-folded `_target_quality` into `_quality` and cleared `_propose_latency`
— its own comment calls that "exactly what `configure` must not do". A raw `handle(Set)`
stream does not. `_request_quality` is edge-guarded, so this only bites when the host's
store disagrees with the adapter's held quality value — but when it does, the result is a
**latency renegotiation mid-bounce**, which hosts handle badly. Narrow trigger, bad outcome.

*Mitigation:* the additive `Hard{std::span params}` form restores the fold for free.

### Medium

**4. The trigger reads an unvalidated host value.** In `Render`, `result` from
`transportStateProc2` / `CallHostTransportState` is `[[maybe_unused]]` — the status is never
checked. Same class as the Galaxy Brain tempo-0 NaN. On failure `sample_pos` stays `0`, so
`0 + nFrames > 0` fires immediately, which degrades safely to committed behaviour. But a
host returning a large negative defers to the 16-block bound (~186 ms of un-cleared realtime
audio), and NaN through `std::llround` is UB.

**5. Store semantics differ per wrapper.** VST3's is `_host_values` in **knob** space, so a
restatement needs `knob_to_plain` — the loop already exists at `audio_effect.cpp:145-153`.
The old "setState doesn't update the cache" bug **is fixed** (line 692 maintains it), so
VST3 is viable. CLAP and AUv3 need the same audit before being touched.

**6. Do not propagate to AAX.** It has **no `Reset::Hard` site at all** — one `Reset::Soft`
at `alg_proc.cpp:390` — because it bounces through a full reconstruct with `Config::params`,
which already solves this. Forcing a restatement there risks colliding with the
ResetFieldData rule and the known private-data wipe.

**7. VST3's gating bug blocks propagation.** `is_offline_bounce` checks
`data.processMode || processSetup.processMode`, but the Hard reset is gated only on
`data.processMode != _last_process_mode`. On a host that sets only the latter the reset never
fires — so the deferral never fires either. Fix that first or the propagation is a no-op.

### Low, but worth recording

- **Unstable sort** (see "Concrete defect found"): the fix lands a restated value immediately
  before an unstably-sorted equal-offset burst, making that latent bug *more* likely to bite.
- **`_param_change_flush_ctr`** is armed once per address (88x) when auto-bypassed or
  disabled. Same value each time, harmless, but it is a state change the committed path did
  not make at that moment.
- **Meters** show the pre-seek level for one block.
- **Cost**: `num_params` x (`GetParameter` + conversion + `handle`) on the render thread per
  discontinuity. Established — the existing resync path does the same — but it now runs on
  every bounce edge and every seek, not only on queue overflow.

### Checked and *not* a risk

A restatement does **not** stomp an in-flight `ScheduleParameter` ramp. The Ramped branch
calls `Super::SetParameter(..., target, off + duration)`, so `Globals()` holds the ramp's
endpoint — and committed `snap()` settles the adapter's ramper to that same target. Same
behaviour either way. Recorded so it isn't re-litigated.

### Structural

`pos >= 0` is keyed to the **project origin**, not to whether the host has spoken. A bar-5
bounce has every block at `pos >= 0`, so it fires at block 0 regardless — the midstream run
came out right because the host had delivered, not because the trigger reasoned about it.
**"First block carrying parameter events"** is the version that is actually about the thing
we care about. Switch triggers *before* multiplying this across four wrappers and many more
hosts, not after.

---

## Recommended order when resumed

1. **Gate the deferral on `Render_mode::Offline`** (risk #1). Non-negotiable; without it the
   change introduces an audible realtime artifact.
2. **Switch to the events-based trigger** — first block carrying parameter events (structural
   risk). Cheaper now than after propagation.
3. **Restore the `_target_quality` / `_propose_latency` fold** (risk #3), most simply via the
   additive `Hard{std::span params}`.
4. **Fix `std::stable_sort`** — independent real defect, ship it on its own.
5. **Fix VST3's `is_offline_bounce` gating** (risk #7) before touching VST3.
6. **Propagate wrapper by wrapper**: VST3, CLAP, AUv3. Skip AAX (risk #6).
7. **Add a per-product idempotence check on `timeline_started()`** (risk #2) — wrapper tests
   cannot catch it.
8. Re-measure in Logic and Ableton after each step.

---

## Concrete defect found: the equal-offset sort is not stable

Independent of everything above, and worth fixing on its own.

The midstream bounce showed Logic delivering **four `Set` events for the same address at the
same offset** (`abs=383716`), arriving in **different order** between the two takes:

```
A: 30.9399962 ... -1.99999809
B: -1.99999809 ... 30.9399962
```

`_set_param` is last-one-wins, and the wrapper sorts with `std::ranges::sort` keyed only on
`(offset, Set-before-Ramp)` — **not stable**, so events comparing equal may come out in any
order. Which value survives is therefore unspecified.

Fix: `std::stable_sort`, or add an arrival counter to `Tagged_event` and use it as the final
tiebreak. Sites: `wrappers/auv2/source/effect.cpp:1160` and
`wrappers/vst3/source/audio_effect.cpp:906`. CLAP appears not to sort at all — check.

---

## Why does Logic pre-roll at all?

No definitive answer; the evidence points at **block-grid alignment**, not priming:

- It is **288 samples in both products regardless of reported latency** (Scissor Hands
  latency 0 -> starts at -288; Galaxy Brain latency 4 -> starts at -284 = -288+4). Latency
  only shifts the reported position, it doesn't buy pre-roll.
- Scissor Hands' block 0 ends **exactly at 0**, which is what a remainder block for grid
  alignment looks like.
- It carries **silence**, so it primes nothing and cannot.
- Ableton does none of it and is fine, which argues it isn't serving any DSP purpose.

Best reading: Logic starts the render at whatever offset makes the following blocks land on
its internal 384/32-sample grid, and the remainder becomes block 0. Worth one experiment
(change **Settings -> Audio -> Devices -> Process Buffer Range** and re-bounce — if the 288
is alignment-derived it should move).

---

## Open questions

1. **Bounce 1 vs bounces 2/3.** Predicted `A != B`, `B == C == D...` with no transport
   activity between. Three consecutive bounces would confirm, and would explain how this
   shipped.
2. **Committed code on a midstream Logic bounce.** Inferred correct (the host chases), never
   directly measured — the midstream run was done with the fix applied.
3. **VST3 / CLAP / AUv3.** Structurally identical to AUv2 (`Reset::Hard` ->
   `clear(); snap();` settling to adapter-held targets). Expect the same defect with a
   shorter window.
4. **Rejected-event logging.** `SetParameter`/`ScheduleParameter` filters (`scope`,
   `element`, `>= num_params`) `continue` silently, so "Logic never calls
   `ScheduleParameter`" is an inference from absent log lines, not proof.
5. **Doctor Vibe** also never converged — it has non-decaying state too. Not yet identified.
6. **Ableton column provenance.** The host-comparison table is labelled "same plug-in, same
   wrapper"; confirm it was the AUv2 build in Live before leaning on it further.

---

## Re-adding the instrumentation

Deleted 2026-09-18 with the rest of the work. It was two pieces, both easy to rebuild.

**`libs/tinyplug/include/tinyplug/tiny_autolog.hpp`** (~145 lines, untracked, header-only).
Compiles away entirely unless `-DTINY_AUTOLOG=1`: `inline constexpr auto on = false;` and
`line(...)` becomes empty, with call sites guarded by `if constexpr (autolog::on)`. Not
RT-safe (stdio + mutex, flushes every line) — diagnostic builds only. Writes
`~/Desktop/autolog_<Plugin>_<pid>_<NN>.txt`, rolling a new file at each offline-render start,
tagged per plug-in via `set_tag` (call it in `Initialize`) so two plug-ins in one process
don't collide. Needs `line(fmt, ...)`, `roll(tag)`, `set_tag(tag)`, `hash_init/hash_add/hash_value`.

Call sites that earned their keep:

| Tag | Where | What |
|---|---|---|
| `INGEST` | `SetParameter` / `ScheduleParameter` | host -> wrapper, with thread id |
| `BLK` | top of `Render` | position, frames, transport, cycle, event count |
| `APPLY` | the event drain | absolute timeline position of each applied event |
| `GLOBALS` | at a discontinuity | the host's full parameter store |
| `ADPT ... phash` | `Effect_adapter::process`, per grid tick | **hash of every distilled DSP param** |
| `OUT ohash` | after `_bypass.process` | per-block hash of the output buffer |

`phash` is the measurement that matters and the one the `auto_probe` never took — the probe
overwrites the distilled value with the raw event value in `Processor::process`, so it can
measure the event stream and the grid but never what the DSP is actually handed. **A green
probe is consistent with a broken plug-in.** Worth widening the probe to emit the distilled
value as its own channel.

**`scratch/auto_probe/compare_autolog.py`** in `all_plugins` (~144 lines, untracked, deleted).
Diffed two logs scoped to offline blocks and reported the `GLOBALS` delta plus the first
`phash`/`ohash` divergence. Verdict rule: *phash first* -> parameter path; *ohash while phash
matches* -> client DSP.

Note it reported the **first** divergence, which for this fix is always block 0 (pre-roll,
expected, never reaches the file) — so its verdict line was misleading here. What actually
matters is the **count**:

```python
import re
def blocks(path):
    out=[]; cur=None
    for line in open(path, encoding='latin1'):
        m = re.search(r'BLK (\d+) frames=(\d+) pos=(-?\d+).*?offline=(\d)', line)
        if m:
            if cur: out.append(cur)
            cur = [int(m.group(1)), int(m.group(3)), None, None] if m.group(4)=='1' else None
            continue
        if cur is None: continue
        m = re.search(r'ADPT spos=.*phash=([0-9a-f]+)', line)
        if m: cur[2]=m.group(1)
        m = re.search(r'OUT ohash=([0-9a-f]+)', line)
        if m: cur[3]=m.group(1)
    if cur: out.append(cur)
    return out
# pass => exactly one divergent block, and it is block 0 at a negative pos.
```

---

## Reproducing

Build instrumented, separate dir so `build-debug` is untouched:

```sh
cmake -S . -B build-autolog -DCMAKE_BUILD_TYPE=Debug \
  -DFETCHCONTENT_SOURCE_DIR_TINYPLUG=/Users/ryan/Developer/tinyplug \
  -DCMAKE_CXX_FLAGS="-DTINY_AUTOLOG=1"
cmake --build build-autolog --target GalaxyBrain_auv2 ScissorHands_auv2 --parallel 8
```

These **auto-install to `~/Library/Audio/Plug-Ins/Components/`** and replace the normal
builds — rebuild from `build-debug` or re-run the installer afterwards to restore them.

Bounce twice, **stopping playback at a different position before each** — the bug hides
whenever the leftover value happens to match. Use Galaxy Brain (SINE) and Scissor Hands
(SAW) as the instrumented pair, and Doctor Vibe (NOISE) as an uninstrumented **control**:
it should keep failing while the pair passes. Compare WAV `data` chunks, not whole files.

> `scratch/auto_probe/data/` is gitignored, and some files in it are duplicate copies of one
> render (`sine_freq_00`/`01` and `saw_lpf_00`/`01` are byte-identical and not even Logic
> output). Check whole-file hashes before trusting a pair — `validate.py` reports
> `IDENTICAL` for a file compared against a copy of itself.
