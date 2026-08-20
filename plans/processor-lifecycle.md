# Processor lifecycle

Status: **designed, scheduled next.** Supersedes the `reset` / `clear` / `snap`
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
    auto reset() -> void;                   // discontinuity: forget history AND land values
    auto snap() -> void;                    // flush: land values only
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

### `reset()` / `snap()`

`clear` folds into `reset`, so the invalid clear-without-snap combination is
unrepresentable and `reset` means what it means in every other framework.

`snap()` survives as the narrow thing it actually is: the flush-only call. It cannot
be replaced by `reset()`, because a VST3 zero-sample process call or a CLAP
`paramsFlush` can arrive **between two audio blocks mid-stream** — forgetting history
there would wipe reverb tails during playback.

Dropping `snap()` entirely was considered and rejected. The common case is a preset
load with the transport stopped: the host delivers everything through flush, no audio.
Without `snap`, the next render glides every parameter from the old preset. For
continuous params that is cosmetic; for mode/switch params it is wrong — a quality
parameter gliding means a **live mode switch with a dip at the head of playback**,
which is the AAX bounce defect generalized to every format and every preset load.

The **bypass-resume** call site (`_was_skipped && !can_skip`) is the weak one and
should be dropped: the rampers are frozen rather than merely behind, but the
un-bypass crossfade already runs over ~20 ms and the glide hides inside it.

### `Event::Any = {Set, Ramp}`

With `Accepted_latency` moved to the context, every remaining alternative genuinely
has a sample offset. That makes the `Tagged_event` / offset / sort machinery apply
uniformly, and it removes the last reason for the explicit rank comparator in the
AUv2 wrapper.

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

## What this deletes

- `adopt_reset_state` and its ordering constraint in
  [alg_proc.cpp](../wrappers/aax/source/alg_proc.cpp). The `Reset_state` snapshot and
  `ResetFieldData` **stay** — that is still how AAX obtains current values at reset
  time — but it feeds `configure` instead of a pre-reset event replay.
- The pre-reset `handle_event` contract, and with it the staging-representation
  hazard (a processor holding parameter values before it knows the sample rate).
- `Accepted_latency` from `Render_event`.
- The `clear(); snap();` pairs the wrappers currently issue after `reset`.
- In `Effect_adapter` downstream: `_target_quality` / `_propose_latency` / silencer-dip
  handling at configuration time. Coming up in HQ stops being a *transition to be
  unwound* and becomes *the configuration you were constructed with*. `_request_quality`
  survives untouched for the live case (user hits the button mid-playback → dip, swap,
  propose), which is what it was written for.

## Open decisions

1. **`query(Property::Latency)` tag dispatch vs. plain `latency_samps()`/`tail_samps()`.**
   Tag dispatch is extensible; the named methods are discoverable and compile-enforced.
   Leaning toward keeping the named methods — there are only two, and both are
   mandatory.
2. **Whether `Config` carries render mode.** It would let a processor size differently
   for an offline bounce. Currently `Render_mode` is per-block in `Dsp_context`, which
   is where it belongs for anything that can change mid-stream; AAX now latches it at
   reset regardless. Probably leave it out.
3. **`Event` / `Property` / `Config` namespacing** — whether these become nested
   structs as sketched or stay free types in `tiny::`.

## Migration order

Both repos move together; the interface break is a compile error everywhere, which is
the point.

1. tinyplug: add `Config`, `Latency`, the new `Event::Any`. Keep the old concept
   compiling in parallel if it makes the wrapper work tractable.
2. tinyplug: wrappers, one format at a time. Each supplies `Config::params` from the
   values it already holds. AAX last — it is the one with a genuinely new path.
3. tinyplug: `template/` + `examples/`, then the concept docs.
4. all_plugins: the three adapters (`effect_`, `nugget_`, `garden_`). Only
   `Effect_adapter` has the quality machinery, so it is the only non-mechanical one.
5. Delete the old concept.

## Test

The invariant that catches the whole class of ordering bugs, and is exactly the AAX
path versus the other four:

> `configure(sr, values)` → render **≡** `configure(sr, defaults)` → feed values →
> `snap()` → render

Bit-identical output. Belongs in the offline golden harness. `tests/allocation/`
should also cover `snap()` and `reset()` in isolation, since both must stay
allocation-free while `configure` is the only tier permitted to allocate.
