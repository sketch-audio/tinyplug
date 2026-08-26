# Processor lifecycle

Status: **in progress.** Step 1 has landed — see
[lifecycle-migration-handoff.md](lifecycle-migration-handoff.md) for where the migration
stands, the contract as implemented, and the client migration guide.

Supersedes the `reset` / `clear` / `snap`
three-tier split that landed alongside the wrapper discontinuity hooks. That commit
was a real improvement — every wrapper grew the host reset hook it had never
implemented, and `Resync_params` stopped masquerading as a timed event — but it left
three seams that only close together. This plan closes them.

## What is wrong today

**1. `reset(sr)` is asked to size the processor using information it does not have.**
The processor allocates for a sample rate *and a configuration*, but only the rate is
passed. Every format's wrapper already holds the current parameter values at that
moment — VST3 in the processor, CLAP and AUv3 in `_hostvalues`, AUv2 in `Globals()`,
AAX in the parameter manager — and simply doesn't pass them.

This is invisible in four formats because the processor object survives across
`setActive`/`activate`, so the values are already in it. AAX is the exception: Pro
Tools wipes algorithm private data at every reset
(`AAX_ePrivateDataOptions_KeepOnReset` is declared but *"Not currently
implemented"*), so the processor arrives default-constructed with every value lost.
The current fix injects `Set_param` events *before* `reset`, which works but makes
AAX the one format where `handle_event` precedes `reset` — an unenforceable ordering
constraint with a silent, format-specific failure mode.

**2. `Accepted_latency` is edge-triggered.** It is an event with no sample offset,
meaning "the host's latency changed *this block*". A processor that misses that block
— resuming from bypass, freshly configured — loses the information. Every wrapper
already computes the value before `process` and delivers it at block granularity, so
the event overstates what it actually is.

**3. `clear()` without `snap()` is never wanted.** Every clear site in the wrappers
pairs them. The combination is representable but invalid.

## Target interface

```cpp
struct Config {
    double sample_rate{48000};
    uint32_t max_block_size{};
    std::span<const double> params{}; // plain space, indexed by address. Borrowed.
};

struct Latency {
    uint32_t accepted{};                 // in:  what the host compensates for
    std::optional<uint32_t> proposed{};  // out: framework reads at block end
};

class Processor {
    Processor() = default;                  // trivial, holds nothing

    auto configure(const Config&) -> void;  // allocating, off the audio thread
    auto reset() -> void;                   // discontinuity: total return to configured state
    auto snap() -> void;                    // flush: land what must be exact  (NAME DEFERRED)
    auto handle(const Event::Any&) -> void; // {Set, Ramp} — everything here has an offset
    auto process(Context&) -> void;         // latency in and out

    auto latency_samps() const -> uint32_t;
    auto tail_samps() const -> uint32_t;
};
```

### `configure(Config)`

Replaces `reset(double)`. Named `configure` because "reset" means *return to an
initial state*, which is close to the opposite of *adopt this state*. The signature
change forces every client to update deliberately rather than compiling silently
against shifted semantics.

After `configure` the object is fully defined and processable, and
`latency_samps()` is final for that configuration. The framework reads it and reports
it to the host; that read *is* the negotiation for this path.

Two contracts that are easy to leave unstated and expensive to discover later:

- **`configure` is re-entrant.** VST3 does `setActive(false)` → `setupProcessing(new sr)`
  → `setActive(true)`, and under this plan that is the routine path rather than an
  exceptional one. A second `configure` on a live object must reallocate safely.
- **`reset()` and `snap()` on a never-configured processor are safe no-ops**, not UB.
  With `Processor()` now trivial and holding nothing, a host that calls Reset before
  Initialize — validators do — must not fall off a cliff. The same question exists
  today for `clear()`/`snap()` before `reset(sr)` and is equally unstated.

### `reset()` and the third verb

`clear` folds into `reset`, so the invalid clear-without-snap combination is
unrepresentable and `reset` means what it means in every other framework.

The third verb survives, and its justification is **mechanical, not aesthetic**:

> Ramps only advance inside `process()`, and there are stretches where `process()` does
> not run — `can_skip` bypass, inactive, flush-with-no-audio. Without a call that
> manifests outside of `process`, a delivered value can stay unrealized indefinitely.

That is the resync case, and it is a correctness property.
[DSPKernel.hpp:169](../wrappers/auv3/source/extension/DSPKernel.hpp) already states it
in a comment: *"a client reading realized state (e.g. an open editor) shouldn't see
stale values for the whole bypassed/inactive stretch."* An earlier draft of this plan
justified the verb by the preset-load case instead; that is a taste argument and
therefore contestable, where this one is not.

It also cannot be replaced by `reset()`, because a VST3 zero-sample process call or a
CLAP `paramsFlush` can arrive **between two audio blocks mid-stream** — forgetting
history there would wipe reverb tails during playback.

Expressing flush as `process()` with `num_frames == 0` was considered and rejected:
advancing rampers by zero samples does not land them, so it would require "a zero-frame
process manifests," a strictly worse contract than a named verb.

**The two verbs differ in kind, not in depth.** This is what rules out collapsing them
into one tagged call:

- `reset()` is **total**. It carries the convergence obligation below — every smoother
  lands, including a long musical one. Reset fires when no audio is flowing, so this
  costs nothing audible.
- The third verb is **deliberately partial**. It lands what must be exact now —
  anything feeding `latency_samps()` or structural configuration — and explicitly
  permits a long musical glide to survive. A ten-second delay-time smoother is not
  obliged to jump.

That second contract is the one that matters in practice: an all-or-nothing "manifest
every value with no glide" forces a global choice between a mode-switch artifact and a
delay-time jump. Narrowing it makes a surviving glide *conforming* rather than a
quiet violation.

**The convergence obligation.** Logic's bounce enters render through `reset()` with no
reconfigure; AAX's enters through a full reconstruct. For those to be bit-identical:

> `reset()` must leave the processor in the state `configure(sample_rate, current_values)`
> would have produced.

Values are already equal — the host set them. **Shape** is not automatic: if a
structural parameter moved during the live session, the processor reached its current
shape via the live path (propose latency → renegotiate → swap → dip). Whether that is
bit-identical to a freshly configured one is a property of the kernel's live-swap code,
which nothing in this interface enforces. Hence writing it down.

**Naming — deferred.** `snap` is the only word nobody uses to describe the operation:
four wrappers say *"settle"* in the comment directly above the call
([aax:329](../wrappers/aax/source/alg_proc.cpp),
[vst3:458](../wrappers/vst3/source/audio_effect.cpp),
[auv3:214](../wrappers/auv3/source/extension/DSPKernel.hpp),
[clap:195](../wrappers/clap/source/plugin.cpp)); others say *"manifest"* / *"realized"*.
`snap` also collides with `_snapshot_knob_params()` in the same file while meaning the
inverse (a snapshot reads state out), and it promises *instantaneous*, which the
narrowed contract above explicitly does not. Candidates ranked: **`settle`** (already
the de-facto word; `Host_bypass` uses "settled" for its own crossfade), `land`,
`realize`, `manifest`. Rejected: `flush` (names the occasion, not the action — the
exact inversion this design forbids), `apply` (collides with the undo `apply<>`
template), `commit`, `resolve`, `end_ramps` (over-specifies; a kernel may settle things
that were never ramps). Not blocking; `snap` stays until someone decides.

**The tag/variant alternative was considered and rejected.** `reset(Hard)` /
`reset(Soft)` is the strongest form — an *ordinal* axis, so unlike a component-axis enum
it does not re-open the invalid history-without-values state. It still loses on three
counts: the two operations differ in kind rather than depth (above), so a strength dial
invites a kernel author to implement the partial one as total; hard/soft already means
something else in this project (*soft bypass* — crossfade-and-tail vs immediate cut, see
`Host_bypass` and CLAUDE.md), and every wrapper has `_processor` and `_bypass` on
adjacent lines; and hard/soft is relative rather than descriptive, which is tolerable in
internal code and worse in a user-facing concept.

### `Event::Any = {Set, Ramp}`

With `Accepted_latency` moved to the context, every remaining alternative genuinely
has a sample offset. That makes the `Tagged_event` / offset / sort machinery apply
uniformly, and it removes the last reason for the explicit rank comparator in the
AUv2 wrapper.

This is also why **no preset-load marker joins the variant** — see "Host loads" below.

### `Latency` in the context

Out through the context (already true of `propose_latency`), back in through the
context. Level-triggered rather than edge-triggered: `accepted` is always valid and
always readable, so a processor that skipped a block still sees the truth. The
kernel's side becomes one line at the top of `process` instead of an event handler
plus an ordering constraint plus an assert about delivery timing.

The framework clears `proposed` before each `process`, or a stale proposal re-fires
every block. It stays `optional` so "no proposal" is distinct from "propose zero".

`latency_samps()` is *not* redundant with `Latency::accepted` — the query is the
processor's truth, `accepted` is the host's. They diverge during negotiation, and that
divergence is the point.

## What the lifecycle does not promise

**Render mode must not influence allocation or latency.** Parameters are structural
inputs and arrive at `configure`. Render mode is an advisory per-block flag and never
does. This needs saying precisely because AAX makes the opposite look workable: it
latches offline into `Reset_state` and adopts it before `reset`
([alg_proc.cpp:92](../wrappers/aax/source/alg_proc.cpp)), then holds it constant for the
whole configuration ([alg_proc.cpp:324](../wrappers/aax/source/alg_proc.cpp)). A kernel
that sized for offline would be honoured on AAX and silently ignored on the other four
— exactly the divergence this plan exists to remove.

Per-format difference to document alongside the existing AAX block-start-delivery and
AUv2-coalescing notes: **AAX delivers a mode that is constant per configuration; the
other four can change it mid-stream.**

Optional tidying, not a fix: the four non-AAX wrappers could seed their initial
`_last_render_mode` from the off-thread signal at configure time so block 1 of a bounce
does not fire a redundant edge. No reallocation involved; the redundant edge is
currently harmless because it clears at a point you would want cleared anyway.

## Host loads: transport-conditioned, and intended

The current behaviour is coherent but written down nowhere, which makes it a prime
candidate for someone to "unify" and break. In CLAP, `_update_state` does not snap; it
routes each value through `_handle_user_action`
([plugin.cpp:517](../wrappers/clap/source/plugin.cpp)) out to the host. Where the values
come *back* is what decides:

- **Transport running** → they arrive as input events in `process()` → no snap → **glide**.
- **Transport stopped** → they arrive via `paramsFlush` → `_from_flush` drain → snap at
  [plugin.cpp:1251](../wrappers/clap/source/plugin.cpp) → **land**.

Glide while audio is flowing, land when there is no audio to artifact. That is the
right answer and it fell out of the flush/process split rather than being designed.
Keep it, and state it.

**No processor-side preset notification.** The escape hatch already exists:
`Host_preset_loaded::add_param` applies an editor-owned marker parameter through the
normal host→processor path, so a kernel that genuinely wants load-awareness watches that
address and acts at the top of its next `process`. No new framework surface, and
`Event::Any` stays `{Set, Ramp}` — a `Preset_loaded` marker event would re-break the
"everything here has a sample offset" property this plan is buying. This holds the
principle that the processor is told what to *do*, not what *happened*.

## AAX: make the accepted latency survive the reconstruct

The channel already exists and is being ignored. `Reset_state::runtime` is a full
`Runtime_packet` ([alg_context.hpp:90](../wrappers/aax/source/alg_context.hpp)) carrying
`latency_seq` and `accepted_latency` alongside `offline`; the data model maintains both
on `SignalLatencyChanged`
([parameters.cpp:415](../wrappers/aax/source/parameters.cpp)) and is never wiped — only
the algorithm's private data is. `adopt_reset_state` reads `.offline` and drops the
other two.

Two consequences today:

- **`st->accepted_latency` is seeded from the processor's opinion, not the host's.**
  [alg_proc.cpp:165](../wrappers/aax/source/alg_proc.cpp) sets it from
  `processor.latency_samps()`. That conflates the two things this plan insists on
  keeping apart. Correct on a genuine add; wrong on a reconstruct, where the host
  already has an accepted value.
- **`st->latency_seq` comes up 0**, so block 1 of every reconstruct fires the seq guard
  against the host's live sequence, issuing a redundant `Accepted_latency` and running
  the assert. Harmless only because the values happen to agree.

The change: latch both fields in `adopt_reset_state`, and make the existing seeding in
`construct_instance` conditional on the documented sentinel.

- `latency_seq == 0` (genuine add) → seed `accepted_latency` from `latency_samps()`,
  push the proposal. Unchanged.
- `latency_seq != 0` (reconstruct / bounce edge) → adopt the host's accepted value and
  sequence; `bypass.set_latency` follows whichever won.

Block 1 then sees a matching sequence, fires nothing, and `Latency::accepted` is the
host's truth from sample zero. It also moves the check to where it is diagnosable:
after `configure`, if `latency_seq != 0`, `latency_samps()` should equal the host's
accepted latency. A failure there says something specific — either `Config::params`
failed to restore the structural mode, or the host clamped what we proposed.

To confirm in Pro Tools rather than from the source: that the accepted latency is stable
across a bounce edge and that `ResetFieldData` sees the post-acceptance value. The
`offline` field's own comment notes the ports are stale at that moment while
`ResetFieldData` is current, so the mechanism is right — but latency has a longer
negotiation tail than a bool. [latency_demo](../examples/latency_demo/) plus an
HQ-style parameter would exercise it.

## What this deletes

- `adopt_reset_state`'s ordering constraint in
  [alg_proc.cpp](../wrappers/aax/source/alg_proc.cpp). The `Reset_state` snapshot and
  `ResetFieldData` **stay** — that is still how AAX obtains current values at reset
  time, and it grows the latency fields above — but it feeds `configure` instead of a
  pre-reset event replay.
- The pre-reset `handle_event` contract, and with it the staging-representation
  hazard (a processor holding parameter values before it knows the sample rate).
- `Accepted_latency` from `Render_event`.
- The `clear(); snap();` pairs the wrappers currently issue after `reset`.
- The bypass-resume call site (`_was_skipped && !can_skip`). The rampers are frozen
  rather than merely behind, but the un-bypass crossfade already runs over ~20 ms and
  the glide hides inside it.
- In `Effect_adapter` downstream: `_target_quality` / `_propose_latency` / silencer-dip
  handling at configuration time. Coming up in HQ stops being a *transition to be
  unwound* and becomes *the configuration you were constructed with*. `_request_quality`
  survives untouched for the live case (user hits the button mid-playback → dip, swap,
  propose), which is what it was written for.

## Resolved decisions

1. **`query(Property::Latency)` tag dispatch vs. plain `latency_samps()`/`tail_samps()`**
   — **keep the named methods.** There are only two and both are mandatory;
   discoverable and compile-enforced beats extensible here.

2. **Whether `Config` carries render mode** — **no.** `configure` allocates, so it needs
   a *quiescent* point, and AUv2 offers none at the mode edge: the offline signal arrives
   via `kAudioUnitProperty_OfflineRender`, which is explicitly settable while
   initialized with no guarantee the render thread has stopped, and Logic's bounce
   empirically delivers only `Reset()`. Adding mode to `Config` would not remove the
   asymmetry, only relocate it — four formats coming up configured-for-offline and AUv2
   coming up realtime-then-cleared, i.e. 4-vs-1 instead of today's 1-vs-4. The contract
   under "What the lifecycle does not promise" replaces the mechanism.
   (For the record: VST3 *does* expose it off the audio thread as
   `ProcessSetup::processMode`; the wrapper reads the per-block `data.processMode` at
   [audio_effect.cpp:362](../wrappers/vst3/source/audio_effect.cpp) instead. Irrelevant
   to configure-time sizing for the reason above.)

3. **`Event` / `Property` / `Config` namespacing** — still open. Nested structs as
   sketched, or free types in `tiny::`. Decide during step 1.

## Migration order

Both repos move together; the interface break is a compile error everywhere, which is
the point. **One step at a time — do not bundle.**

1. **tinyplug, minimal:** add the `Config` struct to the existing header and rename
   `reset(double)` → `configure(Config)`. Nothing else. Latency, the `Event` reduction,
   and the third verb's contract come later. Keep the old concept compiling in parallel
   if it makes the wrapper work tractable. Settle open decision 3 here.
2. tinyplug: wrappers, one format at a time. Each supplies `Config::params` from the
   values it already holds. AAX last — it is the one with a genuinely new path.
3. tinyplug: latency onto the context; `Accepted_latency` out of `Render_event`; the
   AAX `latency_seq` survival change.
4. tinyplug: `template/` + `examples/`, then the concept docs — including the narrowed
   third-verb contract, the convergence obligation, and the render-mode contract.
5. all_plugins: the three adapters (`effect_`, `nugget_`, `garden_`). Only
   `Effect_adapter` has the quality machinery, so it is the only non-mechanical one.
6. Delete the old concept.

## Test

Two invariants, not one. An earlier draft stated a single equivalence that quietly
conflated them.

**1. Value delivery — restricted to non-structural parameters.**

> `configure(sr, values)` → render **≡** `configure(sr, defaults)` → feed values →
> snap → render

Bit-identical, *for all parameters that do not affect sizing or latency*. This catches
the ordering-bug class: a `configure` that ignores `Config::params`, an AAX path that
drops the `Reset_state` snapshot, a third verb that glides instead of landing. The
`automation_tester` probe shape — parameter value straight out as DC — makes a dropped
or one-block-late value visible at sample zero. Belongs in the offline golden harness.

The restriction is not a weakness in the test, it is the design: for a *structural*
parameter the two paths are **required** to differ. Path B must renegotiate latency and
cross-fade through the swap; path A must not. That divergence is the feature, and
stating the quantifier is what stops someone from "fixing" it.

**2. Configuration finality — the invariant that justifies `Config::params`.**

> For a structural parameter at a non-default value: after `configure(sr, {P = v})`,
> `latency_samps() == L(v)` immediately, and rendering N blocks yields
> `Latency::proposed == nullopt` on every block.

An assertion about the negotiation, not about the samples. Needs no golden audio, so it
belongs in tinyplug's own Tier 1; [latency_demo](../examples/latency_demo/) is the
natural fixture and has to change anyway.

**3. Convergence — Logic versus AAX.**

> `configure(sr, V)` → reset → render **≡** `configure(sr, defaults)` → live-set V with
> renegotiation → reset → render

This is the `reset()` convergence obligation made executable, and it is exactly the two
bounce entry paths. Both sides run in-process, so it needs no golden-audio rig.

`tests/allocation/` should also cover the third verb and `reset()` in isolation, since
both must stay allocation-free while `configure` is the only tier permitted to allocate.
