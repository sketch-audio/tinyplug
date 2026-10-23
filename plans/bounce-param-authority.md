# Plan: Reproducible bounces — `Restart` replaces `Hard`/`Soft`, and offline renders hold until the timeline

> Status: **in the tree, 2026-10-22, unmeasured in a real DAW.** Steps 1–4 of "Order of work" are done:
> `Reset::Restart{params}` (`Soft` gone), `process::Stream_sync` in every wrapper but AAX, the hold in AUv2,
> bounce profiles in the CLAP, VST3, AUv2 and AUv3 fake hosts, and the contract checks (tools/contract_checks) on every demo. What remains
> is real Logic (step 5), the all_plugins port (step 6), and the decisions below. Replaces the plan deferred on
> 2026-09-18 (diagnosis and evidence kept below) and settles the reset-API rethink from 2026-10-01.

## Goal

A plug-in built on tinyplug renders **perfectly repeatable bounces by default**, given a host that provides a
reproducible environment. The property is **history independence**: two offline renders of the same range,
started after different histories (playhead parked elsewhere, other bounces before it), are bit-identical from
the timeline origin on.

The defect that breaks it today is Logic's pre-roll: a render from the timeline origin starts ~288 samples before
0, and for those samples Logic delivers no automation while its own parameter store still holds whatever the last
playhead locate left. The plug-in renders on undefined parameter state. Fixing that motivated the reset rethink,
so both are one plan.

## Principles

- **The processor is dumb.** It receives what it needs to render correctly and nothing about why: no "this was a
  preset", "a session load", "a bypass", "a bounce". Two kinds of input reach it:
  - **events** (`handle`): live, at a frame offset. How it realizes them (ramps, smoothing, jumps) is its policy.
  - **commands** (`reset`): block-boundary facts it must adopt for correctness, each with a contract a test can
    check.
- **A command is issued only where correctness needs one**: the processor's input stream is discontinuous, or the
  host made a decision the processor must adopt. Everything else is events.
- **The framework owns when; the processor owns how.** Which situations are discontinuities, and when the values
  are known, is wrapper knowledge. It moves out of five hand-rolled copies into one helper.

## Design

### 1. The processor API

```cpp
struct Reset {
    // Your input stream is discontinuous: a seek, a host reset, the render-mode edge, resuming after the bypass
    // skip. Output from here on must not depend on anything before, and the parameters are exactly `params`
    // (plain space, indexed by address, borrowed for the call, always filled).
    //
    // Owes: the processor ends where `configure(sr, params)` would leave it, except `latency_samps()`, which keeps
    // the accepted value. If `params` imply another latency, propose it from `process`; the host owns the number.
    // "Forget history" means transient processing state only: recorded content (a looper's loop, a loaded sample)
    // is persisted state, not history, and survives.
    struct Restart { std::span<const double> params{}; };

    // The host accepted a latency proposal: `latency_samps() == samples` on return. Exactly once per acceptance.
    struct Latency { std::uint32_t samples{}; };

    using Any = std::variant<Restart, Latency>;
};
```

- **`Soft` is deleted.** Every case it served is a live change (see the mapping), so it becomes events and the
  processor's own smoothing. A processor no longer judges "what must land exactly now".
- **`params` is always filled**, never optional. An optional span would give every processor two code paths and
  make the contract ("equals `configure(params)`") unconditional only half the time. Every wrapper holds the
  values.
- **Carrying the values removes the call-order trap.** Today the wrapper sends `Set`s and *then* `Hard`, and
  `Hard` settles whatever the processor holds; reversed, it settles stale values. That order was load-bearing in
  the old fix. It also restores the old `_adopt_params` latency fold for free (old risk 3).
- **Per block, in this order:** `Latency`, then `Restart`, then the block's events. The usual processor pattern is
  one private `_adopt(params, forget_history)` that both `configure` and `Restart` call.

### 2. What `params` holds

The host's stored values, in plain space, with **this block's offset-0 `Set`s folded in** and consumed. `Ramp`s
carry only a target, so they stay events. Folding is what lands the first automation point of an in-band format
(VST3, CLAP, AUv3), which today arrives after `Hard` and glides in from stale.

| Format | Store | Space |
|---|---|---|
| AUv2 | `Globals()` | host |
| VST3 | `_host_values` | knob |
| AUv3 | `_hostvalues` (DSPKernel) | host |
| CLAP | `_hostvalues` | host |
| AAX | `Alg_state::shadow` | plain, already what the algorithm has seen |
| Web | the processor module's param mirror | plain |

`num_params` is compile-time, so the vector is a stack array in the render call: no member state.

### 3. When: one helper, `Stream_sync`

Header-only in `libs/tinyplug`. Each wrapper reports facts per block; the helper returns what to do. It replaces
`_needs_clear`, `_last_render_mode` / `_last_process_mode`, `_was_skipped` and `_needs_resync` in every wrapper.

```cpp
struct Facts {
    bool host_reset{};                       // Reset(), setProcessing, clap reset, the page
    process::Render_mode render_mode{};      // resolved: VST3 reports data OR setup processMode
    bool skipping{};                         // Host_bypass::can_skip_effect()
    std::optional<std::int64_t> position{};  // timeline sample position, if the host gave a valid one
    std::uint32_t frames{};
};

enum class Action {
    Run,              // process normally
    Hold,             // offline only: silence out, fold events into the store, don't call the processor
    Restart,          // seek-like: Restart, clear notes, clear and snap the bypass
    Restart_resume,   // after the skip: Restart only; the bypass is mid-crossfade and Note_io already sent all-off
};
```

This fixes by construction the VST3 bug where the reset keys on `data.processMode` alone, and the per-format
inconsistencies (state loads land with `Soft` in CLAP, as plain events in VST3). It gets a host-free unit test:
facts in, actions out.

### 4. The fix: hold offline renders until they reach the timeline

1. In an **offline** render, a discontinuity (host reset, render-mode edge) puts the stream in **Hold**. The
   wrapper still pulls input, writes silence and folds events into its store, but doesn't call the processor.
2. The hold ends at the first block reaching the timeline: `position + frames > 0`. In Logic that is the first
   block the host has sent values for.
3. There the helper issues `Restart{params}` and the block processes normally.

A midstream render reaches the timeline at block 0 and restarts at once, as today. **Realtime never holds**, which
removes old risk 1 (an audible deferral when pressing play from bar 1).

Why it works: the pre-roll is the only window where the processor would run on stale values. Holding it back means
nothing advances on them: not the ramps, not the processor's smoothers, not non-decaying state (Galaxy Brain's
carrier phase). Transport-start logic fires once, at a real position, which removes old risk 2 (a second
`timeline_started()`, the first on a stale position). The old fix ran pre-roll then wiped it; the hold gives the
same output without the processor ever seeing the stale block.

Guards:
- **Real content ends the hold at once.** If any input or sidechain sample is non-zero, or a note arrives, the
  helper restarts at that block instead of waiting. So the hold can never delete material: if negative time is
  silence (Logic's default project) it runs to position 0 as designed; if anything real arrives early (content
  before 0, a position shifted the wrong way under latency) it ends there, and behaviour is no worse than today.
  "Negative time isn't the song" stops being an assumption about every host and becomes a checked condition.
- **No valid position** (status ignored today, 0, NaN, garbage): restart immediately, which is today's behaviour.
- **A cap on the hold** (about one second of frames), so a host stuck reporting negative positions can't produce a
  silent bounce. Offline, holding costs nothing audible.
- **Notes during the hold are dropped.** The `Restart` sends all-notes-off anyway, and Logic sends none there.

**Scope: AUv2 first; widen only on evidence.** AUv3 in Logic very likely pre-rolls the same way: measure, then
enable. VST3 and CLAP only if a host is found that renders from negative positions without sending values. AAX
stays out: it rebuilds the processor through `configure` with `Config::params`, which already solves this.

## Mapping from today

**`Hard` → `Restart`** (with the hold in an offline render, where the format has it):

| Site | Trigger |
|---|---|
| [auv2 effect.cpp:1248](../wrappers/auv2/source/effect.cpp) | host `Reset()` |
| [auv2 effect.cpp:1322](../wrappers/auv2/source/effect.cpp) | render-mode edge |
| [vst3 audio_effect.cpp:396](../wrappers/vst3/source/audio_effect.cpp) | `setProcessing` |
| [vst3 audio_effect.cpp:524](../wrappers/vst3/source/audio_effect.cpp) | `processMode` edge, now on the resolved mode |
| [auv3 DSPKernel.hpp:216](../wrappers/auv3/source/extension/DSPKernel.hpp) | host reset |
| [auv3 DSPKernel.hpp:255](../wrappers/auv3/source/extension/DSPKernel.hpp) | render-mode edge |
| [clap plugin.cpp:118](../wrappers/clap/source/plugin.cpp) | `clap_plugin::reset` |
| [clap plugin.cpp:179](../wrappers/clap/source/plugin.cpp) | render-mode edge |
| [web module.cpp:244](../wrappers/web/source/processor/module.cpp) | the page's reset |

**`Soft` after the bypass skip → `Restart_resume`.** The processor missed a stretch of input, which is a gap like
a seek. Freezing state across it brings back a stale tail (a compressor's gain reduction, an LFO mid-cycle, a
delay's pre-bypass echo) and makes output after an automated un-bypass depend on where the render started. The
bypass crossfade brings the wet signal in from the cleared state, so nothing clicks. A product that wants its tail
kept should not be skipped at all (an opt-in that keeps processing while bypassed, 100% dry); left out until one
asks.

| Site |
|---|
| [auv2 effect.cpp:1334](../wrappers/auv2/source/effect.cpp), the `resumed_from_skip` trigger |
| [vst3 audio_effect.cpp:617](../wrappers/vst3/source/audio_effect.cpp) |
| [auv3 DSPKernel.hpp:280](../wrappers/auv3/source/extension/DSPKernel.hpp) |
| [clap plugin.cpp:261](../wrappers/clap/source/plugin.cpp) |
| [aax alg_proc.cpp:469](../wrappers/aax/source/alg_proc.cpp) |
| [web module.cpp:308](../wrappers/web/source/processor/module.cpp) |

**Other `Soft` → deleted; the values go as events.** Nothing reads realized state between `process` calls (meters,
blocks and the state document publish only from `process`), so landing early buys nothing. A bounce right after
any of these opens with `Restart{params}`, which lands everything regardless.

| Site | Trigger |
|---|---|
| [auv2 effect.cpp:1334](../wrappers/auv2/source/effect.cpp), `needs_resync` | queue overflow; the re-read values still go out as `Set`s |
| [auv2 effect.cpp:1334](../wrappers/auv2/source/effect.cpp), `skipped_while_bypassed` | values changed while bypassed |
| [vst3 audio_effect.cpp:632](../wrappers/vst3/source/audio_effect.cpp) | flush block carrying events |
| [auv3 DSPKernel.hpp:230](../wrappers/auv3/source/extension/DSPKernel.hpp) | values changed while bypassed |
| [clap plugin.cpp:139](../wrappers/clap/source/plugin.cpp) | resync after an overflow or sleep; the `Set`s stay |
| [clap plugin.cpp:274](../wrappers/clap/source/plugin.cpp) | flush block carrying events |
| [clap plugin.cpp:1378](../wrappers/clap/source/plugin.cpp) | state load or `paramsFlush` |

AUv2's `_bypass_epoch` / `skipped_while_bypassed` machinery stays: `SetParameter` doesn't queue while bypassed,
and the effect can be bypassed but still crossfading, so values changed then are restated from `Globals()` as
`Set`s. Only its `Soft` went.

**`Latency`**: every site unchanged.

**Processors** (demos; all_plugins has 129 `Reset::` sites in 63 files, most behind its shared adapter):

| Today | Becomes |
|---|---|
| `Hard` handler | `Restart` handler that also adopts `params` (those used to arrive as `Set`s first) |
| `Soft` handler | deleted |
| `Latency` handler | unchanged |

## Verification

1. **A contract check for processor authors**, header-only so products run it downstream. Fuzzed histories,
   bit-compared output:
   - `configure(a)`, any history, `Restart{b}` ≡ `configure(b)`, when `a` and `b` imply the same latency
   - `Restart{b}` twice ≡ once
   - `Latency{n}` leaves `latency_samps() == n`

   This catches what no wrapper test can: per-product state that survives a restart (Galaxy Brain's carrier,
   Doctor Vibe's unidentified one, a non-idempotent `timeline_started()`).
2. **`Stream_sync` unit test**: fact sequences in, actions out, including hold, cap, missing position and
   realtime-never-holds.
3. **A history-sensitive probe**: a mode in `automation_demo` that smooths `Value` and integrates it into a phase
   accumulator, so one stale sample changes the output from then on.
4. **Bounce profiles in `auv2_host`** (which already drives the offline flag and a transport position). One
   scenario per profile: two bounces after different histories are bit-identical from position 0, and equal a
   freshly configured instance.
   - **Logic from the origin**: ~288 samples of negative pre-roll, `Globals()` stale, `SetParameter` between
     sub-blocks from the first block reaching 0. **Red today**, green after the fix.
   - **Logic midstream**: values sent before block 0. Must stay green.
   - **Ableton**: no pre-roll, store correct before the render. Must stay green.
   - **Logic realtime play from bar 1** (`pos = -428`): must not hold.

   A profile encodes what we believe Logic does; it guards the policy, it doesn't prove Logic does it.
5. **Real Logic, by hand** (AUv2, then AUv3): the latency/click test below, three consecutive bounces, a
   midstream bounce, a track freeze, and one bounce with a different Process Buffer Range (if the 288 moves, it is
   grid alignment). Compare WAV `data` chunks, not whole files. Two products, two latencies and one Logic version
   is a small sample; widen it before enabling the hold in other formats.

## Order of work

1. ~~Probe, bounce profiles and the contract check.~~ Done. Each was checked against a deliberately broken
   build: the AUv2 Logic-origin and second-bounce profiles fail with the hold off; the CLAP and VST3 profiles
   fail without the offset-0 fold; `check_restart` fails on a processor that keeps its phase.
2. ~~The API.~~ Done, with MIGRATION.md.
3. ~~`Stream_sync` in every wrapper.~~ Done (AAX keeps its own skip resume). The AUv3 profile found an ordering
   bug on the way: values set off the render thread were delivered after the render's own events, so an older
   value could overwrite a newer one. The kernel now delivers them, and restarts, in `begin_block`, before any
   event. The TSan chaos runs found one of the refit's own: AUv2 and VST3 decided to skip the processor before
   a stream restart snapped the bypass, so a block un-bypassed at a discontinuity mixed in an unwritten output
   buffer (NaN). Both ask `can_skip_effect()` again after the restart now. Review found another: VST3, AUv2
   and web named a block's notes before restarting, and the restart's `Note_io::clear` dropped those names, so
   a note-on in a restart block (bar 1 of an instrument bounce) lost its note-off. They restart first now, and
   the AUv2 and VST3 hosts check it. A discontinuity inside a hold no longer restarts the hold's cap.
4. ~~The hold, AUv2 only.~~ Done.
5. Real Logic measurements; widen the hold per format on evidence.
6. Port all_plugins ([client-port.md](client-port.md)): 129 `Reset::` sites in 63 files, most behind its shared adapter. Add `make_contract_checks` for
   every product; Galaxy Brain's carrier and Doctor Vibe's unidentified state should show there.

## Considerations

1. **Is the reported position in input time? The biggest risk.** The hold assumes everything before position 0 is
   silence, measured for block 0 at latencies 0 and 4. If Logic shifts reported positions by the plug-in's latency,
   a large-latency plug-in could hold while real input from bar 1 arrives, and the hold would delete it (the data
   hints at a shift: latency 4 reports -284 against -288). Test: bounce `latency_demo` at several large latencies
   with a click exactly at bar 1; the click must survive at the right sample. Only real Logic can answer it.
2. **"Negative time isn't the song" is an assumption** about the host: right for Logic's default project, wrong for
   a host that puts audio before 0 (a moved project start, an exported count-in). The early exit on real content
   covers the audio; per-format enabling covers the rest.
   **Plug-ins that make sound from silence** (a hiss or drone generator) output silence during a hold instead of
   their pre-roll. Deterministic, and it only matters if pre-roll reaches the file, which the measurements say it
   doesn't.
3. **Hosts that don't send values before a midstream render** aren't fixed, and can't be detected.
4. **In-band formats need their held blocks' events folded into the store.** AUv2 gets it free (`SetParameter`
   writes `Globals()`).
5. **A realtime bounce** isn't marked offline, so it never holds. Logic's realtime play sends every value up front
   (88 events measured), so it probably doesn't need to; unmeasured.
6. **Back-to-back bounces and freeze** may keep the offline flag set and arrive as `Reset()` instead of a mode edge.
   Any discontinuity while offline holds, so that's covered.
7. **The processor's first block may sit slightly before 0** (−28 at latency 4 in the old log: the block where
   Logic's first value arrived). Its transport sees one start, at a real position.
8. **A restart reads the store at the top of its block**, folding in offset-0 `Set`s. Two stores run ahead of
   that: AUv2's `Globals()` holds a `ScheduleParameter` value (or a ramp's endpoint) from the moment it's
   scheduled, and AUv3's `_hostvalues` a ramp's target. A restart in the same block as such an event lands it at
   the block's start rather than its offset. Deterministic, a few samples at most, and Logic never schedules.
9. **Logic's realtime cycle wrap** (~428 stale samples per lap) is monitoring, not a bounce. Out of scope.

## Decisions for Ryan

- **Keep the method name `reset`, or rename it `sync`?** With `Soft` gone the variant is `{Restart, Latency}`, and
  `Latency` was never a reset. Leaning `reset`: every format calls its discontinuity call reset, and the 129 client
  sites change anyway, so the name costs nothing either way.
- **A `process_while_bypassed` opt-in** (follow-up, when a product asks): a processor that wants its tail kept
  across a bypass declares it statically; the framework then never skips it and runs it 100% dry, so there is no
  gap and no `Restart`. The way to give clients control without a "reason" field, which would let a processor
  treat restarts differently by cause and undo history independence.
- **Should `Restart` carry the render mode**, so the processor only ever sees a mode change across a restart? AAX
  already latches it that way.

## Rejected

- **`Settle{params}`** (a second, history-keeping command): every case it would serve is live.
- **An optional `params` span**: see Design §1.
- **Run pre-roll, then wipe** (the 2026-09-18 fix): the processor sees the stale block, fires transport-start
  twice, and in realtime the deferral is audible.
- **Anchor on "the first block carrying parameter events"** (the old plan's structural recommendation): with no
  automation no events ever come, so it needs a timeout, and CLAP/VST3 blocks carry unrelated events (notes).
  "Negative project time isn't the song" is a statement about the project, not about one host's habits.
- **Restating `Set`s before `Hard{}`**: the call-order trap; carrying the values is the same measurement without it.
- **Snap-on-first-value latch in the processor**: hidden per-parameter state in the client.
- **One-block lookahead**: latency and PDC churn. **PLL / position-derived phase**: slewing an audio-rate carrier's
  phase is audible FM; anchoring at one restart is exact.

---

# Diagnosis and evidence (2026-09-16 / 18)

Measured in Logic with AUv2 builds of Galaxy Brain and Scissor Hands, Doctor Vibe as an uninstrumented control.
Code references in this half describe the tree as it was then.

## The defect, precisely

1. At the start of an offline render, the plug-in's parameters **and** Logic's own `Globals()` store hold the
   automation value at the **playhead**, not at the render-start position. The two agree, so no host↔plug-in
   resync can detect it.
2. Logic implements sample-accurate automation by **subdividing the render buffer** (multiples of 32 samples, max
   384; 1024 when no automation is active) and calling `SetParameter` at offset 0 between sub-blocks. It never
   calls `ScheduleParameter`, and delivers from its first 32-sample grid boundary onward. **This is specific to a
   render starting at the timeline origin**: a midstream render's first block arrives carrying events.
3. That first block is rendered on the playhead's value. Its input and output are **bit-exact silence**, but it
   still advances effect state.
4. The playhead is usually parked somewhere different before each take, so the stale value differs, which is what
   makes renders differ. Park it identically and two bounces match while both remain wrong.

At the time, `Hard{}` settled the adapter's own held ramper targets, the residue of every `SetParameter` so far;
`Globals()` was never read. So the dependency was: *the host must deliver the render-start values as events before
the first offline `Render` call.* An event at offset 0 of the first block was already too late: it became a new
target the ramper glided into from stale.

### Hosts already correct

- **Ableton (measured):** two bounces bit-identical. Render starts at pos 0, no pre-roll, the store already holds
  the render-start value at the reset.
- **Logic midstream (inferred):** block 0 carried 4 events with `GLOBALS addr 1 = -1.99999809` in both takes. Not
  directly measured on committed code.

### Why it shipped

On most effects it's a sub-20 ms transient at the head of a file. And bouncing twice without touching the
transport passes, because bounces 2 and 3 both start from bounce 1's end value (predicted, not verified).

## Evidence

- **Automation delivery is identical between takes**: 1357 events (Galaxy Brain) / 1693 (Scissor Hands), same
  values at the same positions. The "delivery race" hypothesis is dead.
- **`GLOBALS` differs in exactly one parameter**, the automated one.
- **Block 0 is silence in, silence out** (bit-identical to an all-zeros hash).
- **Logic has a chase and skips it here.** Realtime play delivers 88 events in the first block (every parameter,
  at `pos=-428`). A cycle wrap delivers 0. An offline bounce start delivers 0: a bounce behaves like a wrap.
- **Cycle on or off made no difference**: Logic reports `cyc=0` during an offline render either way.

| | Logic | Ableton |
|---|---|---|
| Render start | pos **-284** (pre-roll) | pos **0** |
| Block sizes | subdivided 32…384, 1024 idle | fixed **2048** |
| Automation offsets | always `off=0` | real offsets: 4, 260, 332, … |
| First block | `events=0` | `events=1` |
| Stale window | **288 samples** | **4 samples** (= latency) |
| `Globals()` at the reset | playhead value, **stale** | render-start value |
| Two bounces | diverged | bit-identical |

### Why Logic pre-rolls

Best reading: **block-grid alignment**, not priming. It is 288 samples regardless of latency (latency only shifts
the reported position: 0 → -288, 4 → -284), block 0 ends exactly at 0 like a remainder block, it carries silence,
and Ableton does none of it. One experiment would settle it: change Logic's Process Buffer Range and re-bounce.

## The three layers of contamination

| # | Layer | Recovers? |
|---|---|---|
| 1 | Parameter ramps glide from the stale value | yes, after the ramp time (every product's policy is `Linear` or `Instant`) |
| 2 | The effect's own smoothers (coefficient history) | decays: Scissor Hands' 14.7 ms residual |
| 3 | **Non-decaying state**: Galaxy Brain's carrier phase | **never** |

> The invariant: *no state that integrates without decay may advance on unauthoritative parameters.* The hold
> enforces it by not running the processor at all until the parameters are authoritative.

Discrete parameters are `Instant`, so a stale mode doesn't glide, but what the DSP did in the wrong mode can still
feed layer 3.

## Measurements of the old fix

Defer the discontinuity to the first block reaching the timeline, restate from `Globals()`, then `Hard{}`:

| Bounce | Product | WAV `data` chunk |
|---|---|---|
| SINE | Galaxy Brain | **identical** `4009b756ae91` |
| SAW | Scissor Hands | **identical** `473f0e7b0d86` |
| NOISE | Doctor Vibe (control) | differs |

Only block 0 (pre-roll, never in the file) differed internally. The stale values were far apart (`addr 11`: 812
vs 5482, a 6.7× cutoff gap), so this was a hard test. A midstream bounce (bars 5–9) was bit-identical with no
divergence at all. An earlier API-rewrite shape (`Clear`/`Sync{params}`) produced the same internal state hash at
block 1 (`f50ba0ca4437557b`) as the restate shape: carrying the values and restating them are equivalent.

```
BLK 0 pos=-284 events=0
  ADPT timeline_started spos=35412     <- stale transport state: what the hold removes
BLK 1 pos=-28 events=1
  RESTATE+HARD (render-mode change)
  APPLY SET addr=1 val=0 abs=-28
  ADPT timeline_started spos=0         <- the start that should be the only one
```

## Open questions carried over

1. **Bounce 1 vs bounces 2/3**: predicted `A != B`, `B == C == D`. Three consecutive bounces would confirm.
2. **Rejected-event logging**: `SetParameter`/`ScheduleParameter` filters `continue` silently, so "Logic never calls
   `ScheduleParameter`" is an inference.
3. **Doctor Vibe** never converged; its non-decaying state is unidentified. The contract check should find it.
4. **Ableton column provenance**: confirm it was the AUv2 build in Live.

## Instrumentation and reproducing

The 2026-09 measurements used a header-only `tiny_autolog.hpp` (deleted; stdio + mutex, diagnostic builds only,
`-DTINY_AUTOLOG=1`) writing per-render files with these tags: `INGEST` (host → wrapper), `BLK` (position, frames,
event count), `APPLY` (absolute position of each applied event), `GLOBALS` (the host's store at a discontinuity),
`phash` (hash of every distilled DSP parameter per grid tick — the measurement that mattered) and `ohash` (output
buffer). `scratch/auto_probe/compare_autolog.py` in all_plugins diffed two logs over offline blocks; count divergent
blocks rather than reading its first-divergence verdict. tinyplug's `log::Probe` layer (CLAUDE.md, "Logging") is
the realtime-safe home if this is rebuilt.

To reproduce in Logic: bounce twice, **stopping playback at a different position before each** (the bug hides when
the leftover value happens to match). Instrumented builds auto-install to `~/Library/Audio/Plug-Ins/Components/`
and replace the normal ones. Compare WAV `data` chunks: Logic stamps `bext`, `ResU` and `LGWV` per bounce, so
whole-file hashes always differ.
