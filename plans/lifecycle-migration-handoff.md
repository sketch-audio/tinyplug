# Lifecycle migration — handoff

Working document for the in-flight migration described in
[processor-lifecycle.md](processor-lifecycle.md). That file is the design; this one is
where the migration currently stands, what the contract now says, and what to do next.
Delete it when the migration lands.

Status: **step 1 complete and building.** `configure` has replaced `reset(double)` in the
framework and all five wrappers, and the latency handshake has been audited and repaired
across all five. `all_plugins` has **not** been migrated and will not compile against this.

---

## 1. What landed

### `Config` and `configure`

`reset(double)` is gone. [tiny_processor.hpp](../libs/tinyplug/include/tinyplug/tiny_processor.hpp):

```cpp
struct Config {
    double sr{48000};
    std::span<const double> params{}; // plain space, by address. Borrowed.
};
```

`max_block_size` was deliberately left out — easy to add, not needed yet.

Every wrapper now supplies `Config::params` from values it already holds. **The source
space differs per format**, which is the easiest thing to get wrong:

| Format | Source | Conversion |
|---|---|---|
| VST3 | `_host_values` | **knob** → `knob_to_plain` |
| CLAP | `_hostvalues` | host → `host_to_plain` |
| AUv3 | `_hostvalues` | host → `host_to_plain` |
| AUv2 | `Globals()->GetParameter` | host → `host_to_plain` |
| AAX | `Reset_state::coefs` | already **plain** |

AAX's `adopt_reset_state` now fills a caller-supplied `std::span<double>` (pre-seeded with
plain-space defaults) instead of replaying `Set_param` events before `reset`. That deletes
the ordering constraint where AAX was the one format in which `handle_event` preceded
`reset`.

### Latency repairs

- **AAX** — `adopt_reset_state` latches `latency_seq`, `accepted_latency` and
  `reported_latency` from `Reset_state::runtime`. The proposal in `construct_instance` is
  gated on `initial_latency != reported_latency` rather than on `adding_new`. *Fixed an
  active bug: every Pro Tools reset asked the host to renegotiate delay compensation,
  twice per offline bounce.*
- **AUv2** — the `_pqueue` pump moved off the view onto the `Effect`
  ([relay.hpp](../wrappers/apple/relay.hpp)). *Fixed an active deadlock: with
  the editor closed nothing drained the queue, so the handshake stalled permanently and the
  dedupe guard blocked every retry.* `Initialize` now syncs `_reported_latency` and
  notifies directly when `configure` moved the latency.
- **AUv3** — `_latency`/`_reported_latency` zero-initialised and `_latency` made atomic;
  `initialize` clears proposals `configure` supersedes (*fixed a stale-proposal assert
  across a render-resource cycle*); `latency_secs()` split into `peek_latency_secs()` and
  `accept_latency()` so the host reading the property is the accept, not a timer tick; KVO
  posts hop to main.
- **VST3** — `_needs_report` made atomic; `getLatencySamples()` peeks `_pending_latency`.
- **CLAP** — unconditional `configure`, `_once` deleted, `_reported_latency` synced after
  configure with a notify when it moved, notify moved last.

### New file

[wrappers/apple/relay.hpp](../wrappers/apple/relay.hpp) — `Relay`, a GUI-independent GCD
pump. `poll` runs on a global queue (no run-loop dependency), `deliver` hops to main and
only runs when `poll` asks. Shared by AUv2 and AUv3; nothing in it is format-specific.

---

## 2. The `configure` contract

> `configure(Config)` is **sufficient on its own**. `clear` and `snap` are implied. On
> return the processor renders from exactly this configuration with defined output, and
> `latency_samps()` is final for it — no negotiation, no glide up from defaults.

Consequences worth stating separately:

- **`configure` must be deterministic in `(sr, params)`.** Everything that affects
  `latency_samps()` or the processor's shape has to be reconstructible from those two.
  This is not purity — it is AAX showing through: `construct_instance` does
  `st = new (…) Alg_state{}`, so the processor is genuinely default-constructed at every
  Pro Tools reset and anything not in `Config::params` is gone.
- **`configure` is re-entrant.** VST3 does `setActive(false)` → `setupProcessing` →
  `setActive(true)` as a routine path. A second `configure` must reallocate safely.
- **`reset()` and `snap()` on a never-configured processor are safe no-ops**, not UB.
- **`reset()` owes convergence with `configure`** — it must leave the processor where
  `configure(sr, current_values)` would have. Logic bounces via `reset()` alone while AAX
  bounces via full reconstruct; those two paths have to agree bit-for-bit.
- **Render mode must not influence allocation or latency.** Parameters are structural
  inputs and arrive at `configure`; render mode is an advisory per-block flag. AAX latches
  it per-configuration and makes the opposite look portable — it isn't.

---

## 3. The latency contract

1. **The kernel proposes; it never applies.** It keeps rendering at its current latency
   until handed an accepted value, then matches it exactly.
2. **`configure` is a fact, not a proposal.** `latency_samps()` is final for the
   configuration.
3. **Two numbers, two meanings.** `_latency` is what the host holds; `_reported_latency` is
   what we last asked for. They diverge only inside the async negotiation window — which is
   exactly why the dedupe guard cannot be folded into `_latency`. `request_restart` /
   `restartComponent` are asynchronous and blocks keep rendering meanwhile.
4. **Notify whenever the host's number became wrong** — from a proposal *or* a configure.
   Never notify when it didn't move. The `_reported_latency == _latency` comparison after
   configure is what makes bounce entry free.
5. **The getter answers consistently with the last notification.** VST3 declares the change
   *before* the main thread adopts it (the only channel to the controller is
   `outputParameterChanges`, emitted from `process`), so its getter must **lead** — hence
   the peek. Every other format declares at or after the update, so its getter **follows**.
6. **The notification path is never GUI-bound.** The editor may never open.
7. **It fails closed.** A host that ignores the notification leaves the kernel where it is:
   no misalignment, just no change.
8. **`configure` supersedes an in-flight handshake.** A proposal outstanding when the host
   reconfigures is discarded, not applied against the new configuration — `configure` makes
   latency final, so an old proposal would hand the kernel a value it never asked for.
   `_reported_latency` survives the supersede: it records what the host was last told, and
   comparing it after `configure` is what clause 4 rests on. Where the clearing lives:
   VST3 `setupProcessing` · CLAP the fall-through in `activate` · AUv2 `Cleanup` ·
   AUv3 `deInitialize` · AAX `adopt_reset_state` re-seeding from the snapshot.

Where the accept happens: VST3 `setActive` · CLAP `activate` · AUv2 `GetLatency` ·
AUv3 the `latency` getter · AAX the `latency_seq` bump in the `Runtime_packet`.

### Known limits — not supported, by decision

- **Latency derived from anything outside `(sr, params)`.** A worker-loaded IR that changes
  convolution latency works in four formats and silently breaks in AAX, per §2. Buffer-system
  territory.
- **A host that accepts a different latency than proposed.** AAX owns the value and may
  clamp it; the kernel then gets `Accepted_latency{clamped}` and the assert fires. There is
  no rejection path in the protocol.
- **Different oversampling factor for offline bounce.** The active factor would be selected
  by render mode, which is not in `Config`, so `latency_samps()` after `configure` would be
  undefined. Authors must report a constant latency across factors. If this ever becomes a
  requirement, the fallback is asymmetric support (four formats can; AUv2 cannot), not a
  redesign — see resolved decision 2 in the design doc.
- **A latency change requested while the host has PDC disabled (AAX).** The
  `runtime.delay_comp != 0` guard drops the proposal without recording it, and nothing
  re-proposes if PDC is re-enabled. Known bug, unfixed.
- **Sweeping a continuously-variable latency control.** The dedupe only blocks repeats of
  the same value, so a knob sweep produces one host restart per distinct value. No rate
  limit exists. Author's responsibility; undocumented.
- **A host that ignores the notification and then reconfigures.** `_reported_latency` is
  what we last *asked for*, not what the host *adopted*. If a host drops the notification
  and the new configuration lands on the same value the abandoned proposal named, clause 4
  keeps us silent while the host still holds the pre-proposal number. Only reachable for a
  host already out of spec; the alternative — notifying unconditionally on every
  reconfigure — is the AAX bug this migration fixed, so it is not worth the trade.
- **AAX's `ResetInstance` branch with surviving private data** ([alg_proc.cpp](../wrappers/aax/source/alg_proc.cpp))
  reconfigures but skips `bypass.set_latency`, the `accepted_latency` seeding and the
  proposal comparison that `construct_instance` does. `Host_bypass::reset` deliberately
  does not resize its delay lines, so a configure that moved the latency on that path
  leaves the bypass compensation stale and the host untold. Rare — Pro Tools normally wipes
  private data, taking the `else` branch — but it is a divergence inside one format. Wants
  the post-configure latency block factored into a helper both paths call.

---

## 3a. Probing it

**Removed from the wrappers.** AAX, VST3 and AUv3 were instrumented with `log::Probe`
([lifecycle_probe.hpp](../libs/tinyplug/include/tinyplug/lifecycle_probe.hpp)) while the
handshake above was being repaired; CLAP and AUv2 carry none. The probes are stripped so
the lifecycle diff stays readable, but the logging layer and the probe vocabulary are kept
intact — see the "Logging and lifecycle probing" section of [CLAUDE.md](../CLAUDE.md).
Re-add a `log::Probe` member and the calls below when a handshake question needs a trace;
debug builds have the layer on with no setup, and `TINYPLUG_LOG_CATS=lifecycle,latency`
narrows it.

What a trace answers that reading the code does not — i.e. what to re-instrument for:

- **Clause 4, "never notify when it didn't move."** The `_reported_latency == _latency`
  comparison after configure is what makes bounce entry free, and a
  `latency_dropped ... "configure left the host's number correct"` line is the proof it
  fired. Confirmed against clap-validator: a process's first `activate` notifies, and a
  second `activate` on the same instance is silent.
- **Clause 5, whether the getter leads or follows.** `latency_queried` records what the
  host was told and when, relative to `latency_notified`. This is the one open risk noted
  in §6 for VST3's peek.
- **Silent drops.** Every suppression path emits `latency_dropped` with a reason — the
  dedupe guard, AAX's `runtime.delay_comp` guard (the known unfixed bug in §3), a full
  return ring, an AUv3 proposal superseded by `configure`. These are invisible otherwise
  and are the failure mode this protocol has.
- **AAX reconstruction.** `Alg_state` holds its own probe, so every Pro Tools reset shows
  as a new instance number with its own `configure`, which is what makes the
  AAX-versus-everything-else ordering question in §2 readable side by side.
- **The convergence obligation.** `configure(sr, current_values)` versus `reset()` should
  produce identical lifecycle traces. Diffing two logs is the cheapest test of that.

The `latency_mismatch` verb is the assertion in release clothing: it fires where
`assert(latency_samps() == accepted)` would, but survives a non-assert build and names
both numbers. AAX is where it can legitimately fire, since the host owns the value and may
clamp it.

## 4. Landed — `reset(Reset::Any)`

`clear()` / `snap()` are gone, and so is `Accepted_latency`. The processor concept now has
two state entry points split by cost rather than depth:

```cpp
struct Reset {
    struct Hard {};                              // stream restarting: forget history, land everything
    struct Soft {};                              // land what must be exact; history and long glides survive
    struct Latency { std::uint32_t samples{}; }; // host accepted a proposal — adopt it now
    using Any = std::variant<Hard, Soft, Latency>;
};

auto configure(const Config&) -> void;   // allocates, off the audio thread
auto reset(const Reset::Any&) -> void;   // never allocates
```

`Render_event` is down to `{Set_param, Ramp_param}`, so **every** alternative genuinely
carries a frame offset and the `Tagged_event` sort means what it says.

Why the variant rather than a level-triggered `Latency` in the process context, which an
earlier draft of this document proposed: **a variant member is compile-time mandatory.**
A kernel that never reads a context field renders at one latency while the host
compensates for another — permanently misaligned, with no compile error and nothing in a
trace. Omit an arm of an exhaustive visit and it does not build. The self-healing property
the context version was for does not survive contact with the code either: all five
wrappers already latch the accepted value and consume it at the top of `process` before
any skip branch, so the level-triggering exists — at the wrapper layer, where it belongs.

The reservation, recorded rather than argued away: what the three alternatives share is
*delivery shape*, not meaning. `Hard` discards, `Soft` lands, `Latency` does neither — it
adopts a number. And they differ on delivery contract. `Hard`/`Soft` are the framework's
discretion, harmless if issued twice or judged unnecessary; `Latency` is a mandatory
handoff with a post-condition, and the harm is in *not* delivering it. That difference is
invisible once they are siblings in a variant, so it is stated at the type.

Call-site mapping applied across all five wrappers:

| was | now |
|---|---|
| `clear(); snap();` (host reset, render-mode edge) | `reset(Reset::Hard{})` |
| lone `snap()` (flush, resync, bypass resume) | `reset(Reset::Soft{})` |
| `handle_event(Accepted_latency{n})` | `reset(Reset::Latency{n})` |

AUv2's render-mode branch became an `if/else` against the resync branch, since `Hard`
already lands values. Its explicit `event_rank` comparator **stays**: the two-alternative
variant no longer needs it, but [midi-support.md](midi-support.md) adds alternatives back,
and an intransitive equivalence in `std::sort` is UB.

---

## 5. Migrating `all_plugins`

Three adapters in `shared/processor/`. `nugget_adapter.hpp` and `garden_adapter.hpp` are
mechanical. `effect_adapter.hpp` is not, and is worth doing first because it exercises
every clause.

### Mechanical part (all three)

```cpp
auto reset(float sr) -> void   →  auto configure(const Config& config) -> void
auto clear() -> void           ┐
auto snap()  -> void           ┴→ auto reset(const Reset::Any&) -> void
```

Inside `configure`, `const auto sr = static_cast<float>(config.sr);` and adopt
`config.params` before anything that depends on parameter values. `configure` implies both
reset kinds, so the existing trailing `clear(); snap();` stays as private helpers the new
`reset` dispatches to — the *wrappers* stopped issuing the pair, not the adapter.

### `effect_adapter` specifically

The quality machinery is the interesting case, and quality **is a parameter**
(`_set_param` → `_request_quality` at line 626), so it rides in `Config::params` correctly.

**1. Adopt, don't request.** At configure time the quality parameter must set `_quality`
*directly*. Routing it through `_request_quality` would set `_target_quality` and
`_propose_latency`, i.e. schedule a dip and a negotiation — exactly what `configure` must
not do. Add an adopt path that bypasses `_request_quality` for the quality address:

```cpp
auto configure(const Config& config) -> void
{
    const auto sr = static_cast<float>(config.sr);

    // Values first: quality decides which pipelines to size and what latency to report,
    // and `configure` reports rather than negotiates.
    _adopt_params(config.params);      // sets _quality directly
    _target_quality = std::nullopt;
    _propose_latency = false;

    // ... existing allocation, now with _quality already correct ...
    _latency = _latency_samps_for(_quality);
    // ...
    clear();
    snap();
}
```

**2. Delete the `_target_quality` adoption block** (currently lines 73–79). It exists to
make `reset` come up in the mode a pending switch was heading for. With values arriving in
`Config::params`, coming up in HQ stops being *a transition to be unwound* and becomes
*the configuration you were constructed with*. The silencer no longer needs to start dipped.

**3. `_request_quality` survives untouched.** It is for the live case — user hits the
button mid-playback, dip, swap, propose — which is what it was written for and the only
thing the handshake is still needed for.

**4. `Accepted_latency` is gone.** Move that branch out of `handle_event` (lines 189–196)
into the adapter's new `reset`:

```cpp
auto reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&)         { clear(); snap(); },
        [this](const Reset::Soft&)         { snap(); },
        [this](const Reset::Latency& e)    { _latency = e.samples; }
    }, reset);
}
```

`context.propose_latency` is unchanged — only the inbound half moved. The
`settled` computation at line 489 (`!_target_quality && _latency == _latency_samps_for(_quality)`)
keeps working unchanged, and the `quality_actual` meter now reads settled immediately after
a configure — which is the visible symptom of this whole migration working.

### Verifying it

Three tests, per the design doc's Test section. The one that matters most for this adapter:

> `configure(sr, {quality = HQ})` → render **≡** `configure(sr, defaults)` → set HQ live →
> renegotiate → `reset()` → render

That is Logic-vs-AAX bounce entry, and it is what `reset()`'s convergence obligation buys.

---

## 6. Outstanding smaller items

- ~~**CLAP**: `activate` early-returns on `pending_latency.has_value()` alone.~~ Fixed: the
  early return is gated on `sampleRate == _sr`, and the fall-through clears
  `_accepted_latency` so the superseded handshake cannot reach the kernel. The configure
  path already notified on `latency_moved`.
- **Thread-safety pass.** `mSampleRate` in the AUv3 kernel is a plain `double` written in
  `initialize` and read from the pump (safe today only because the pump's lifetime nests
  inside render resources). `_reported_latency` is render-thread-only in most wrappers but
  touched from `Initialize`/`activate` in others. Wants systematic treatment.
- **VST3 `getLatencySamples()` peek** is the one change this session that alters what a host
  reads at a moment it was already reading, and it rests on the documented
  `restartComponent` → `getLatencySamples()` → `setActive` order rather than observed
  behaviour. If PDC ever looks off by exactly one mode switch, revert this first.
- **VST3's configure-time report is load-bearing in Ableton.** Observed: Live changes the
  sample rate with a bare `setActive(0)` → `setupProcessing` → `setActive(1)` — no
  `terminate`, no `initialize` — and never re-queries `getLatencySamples` afterwards. So the
  deferred `_needs_report` → output-param bump → `restartComponent(kLatencyChanged)` path is
  the only way Live learns a configure-time latency change. The trace that established the
  call order ran at latency 0 throughout, so that path itself is **still unverified** — the
  test is LatencyDemo in high mode across a rate change.
- **AUv2 `-Wswitch-default`** forces a `default:` on the `Message_type` switch, so a new
  message kind won't get a compiler nudge there.
- Open decision 3 in the design doc — `Event`/`Config` namespacing — still unsettled.
