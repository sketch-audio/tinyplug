# Migrating a plug-in to the new processor API

Hand-off guide for porting a downstream repo (`all_plugins` and friends) to the processor
API as it stands on `main`. Written to be followed top-to-bottom without reading the design
docs first; the *why* for each step links out.

The interface break is deliberate and total — every change below is a compile error, not a
silent behaviour shift. If it builds, you have almost certainly done it correctly. The two
exceptions are called out under **Traps** at the end; read those before you start.

Design background, if you want it: [processor-lifecycle.md](processor-lifecycle.md) is the
rationale, [lifecycle-migration-handoff.md](lifecycle-migration-handoff.md) is what landed
and why.

---

## 0. What changed, in one screen

```cpp
// BEFORE                                    // AFTER
namespace tiny::plugin {                     namespace tiny::process {

class Processor {                            class Processor {
  void reset(double sr);                       void configure(const Config&);
  void clear();                                void reset(const Reset::Any&);
  void snap();
  void handle_event(const Render_event&);      void handle(const Event::Any&);
  void process(Dsp_context&);                  void process(Dsp_context&);
  uint32_t latency_samps() const;              uint32_t latency_samps() const;
  uint32_t tail_samps() const;                 uint32_t tail_samps() const;
};                                           };
}                                            }
```

Five separate changes, in dependency order:

1. `reset(double)` → `configure(Config)` — parameter values now arrive with the rate.
2. `clear()` + `snap()` → `reset(Reset::Any)` — a closed sum of block-boundary syncs.
3. `Accepted_latency` leaves `Render_event` and becomes `Reset::Latency`.
4. `Render_event` → `process::Event::Any`, and **the render event is now a different type
   from the edit event**.
5. Everything process-side moves into `tiny::process`, the user's `Processor` included.

---

## 1. `reset(double)` → `configure(Config)`

```cpp
struct Config {
    double sr{48000};
    std::span<const double> params{}; // PLAIN space, indexed by address. Borrowed.
};
```

```cpp
auto configure(const Config& config) -> void
{
    const auto sr = static_cast<float>(config.sr);
    _adopt_params(config.params);   // BEFORE anything that depends on parameter values
    // ... existing allocation ...
}
```

**`params` is borrowed** — valid only for the duration of the call. Copy what you keep.

### The contract you are now signing

- **`configure` is sufficient on its own.** On return the processor renders from exactly
  this configuration with defined output. It implies both `reset` kinds; if your old code
  ended with `clear(); snap();`, keep those as private helpers and call them at the end.
- **`configure` is a fact, not a proposal.** This is the clause most likely to be got
  wrong, so it is worth stating as a prohibition rather than a preference:

  > After `configure` returns, `latency_samps()` is the truth for this configuration, and
  > the framework reports it to the host directly. Your kernel must then leave
  > `context.propose_latency` **disengaged on every block** until a *live* parameter change
  > moves a structural parameter.

  Whatever `config.params` implies, you come up already in it — no negotiation, no glide up
  from defaults on block 1, no dip, no cross-fade. The state handed to `configure` **is**
  the truth; you do not ask the host to agree to it.

  The consequence of getting this wrong is not theoretical. A kernel that re-proposes its
  own latency after a configure makes Pro Tools renegotiate delay compensation at *every*
  reset — which is twice per offline bounce, while the host is rebuilding its mixer graph.
  That was a live bug in this framework's AAX wrapper, found by exactly this rule.
- **`configure` must be deterministic in `(sr, params)`.** Anything affecting
  `latency_samps()` or the processor's shape has to be reconstructible from those two. This
  is not purity for its own sake: Pro Tools wipes AAX private data at every reset, so the
  processor is genuinely default-constructed and anything not in `Config::params` is gone.
- **`configure` is re-entrant.** VST3 does `setActive(false)` → `setupProcessing` →
  `setActive(true)` routinely. A second `configure` on a live object must reallocate safely.

### If your processor negotiates latency

This is the part that is not mechanical. At configure time a structural parameter must be
**adopted directly**, not routed through whatever function requests a live change — that
would schedule a dip and a renegotiation, which is exactly what `configure` must not do.

```cpp
auto configure(const Config& config) -> void
{
    _adopt_params(config.params);   // sets _quality (or equivalent) DIRECTLY
    _target_quality = std::nullopt; // no pending transition
    _propose_latency = false;       // no proposal outstanding
    // ... allocation, now with the structural mode already correct ...
    _latency = _latency_samps_for(_quality);
}
```

Delete any block that exists to make `reset` come up in the mode a *pending* switch was
heading for. Coming up in HQ is no longer a transition to be unwound; it is the
configuration you were constructed with. Your live-change path survives untouched — that
is what it was written for.

---

## 2. `clear()` + `snap()` → `reset(Reset::Any)`

```cpp
struct Reset {
    struct Hard {};                              // stream restarting
    struct Soft {};                              // land deferred values
    struct Latency { std::uint32_t samples{}; }; // host accepted a proposal
    using Any = std::variant<Hard, Soft, Latency>;
};
```

The mechanical port keeps your existing helpers and dispatches to them:

```cpp
auto reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&)      { clear(); snap(); },
        [this](const Reset::Soft&)      { snap(); },
        [this](const Reset::Latency& e) { _latency = e.samples; }
    }, reset);
}
```

Then read the contracts and check your helpers actually satisfy them:

| | obligation |
|---|---|
| `Hard` | **Total.** Forget history *and* land every deferred value, including a long musical smoother. Fires when no audio is flowing, so it costs nothing audible. |
| `Soft` | **Deliberately partial.** Land what must be exact now — anything feeding `latency_samps()` or structural configuration. History survives, and a long musical glide is *permitted* to keep gliding. |
| `Latency` | Adopt `samples` immediately. `latency_samps()` must equal it when the call returns. |

Three rules that are easy to miss:

- **`reset` never allocates.** `configure` is the only tier permitted to.
- **`Hard` and `Soft` must not change `latency_samps()`.** Only `configure` and
  `Reset::Latency` may move it. See **Traps**.
- **`reset` on a never-configured processor is a safe no-op**, not UB. Validators call
  reset before initialize.

`Hard` also owes **convergence**: it must leave the processor where
`configure(sr, current_values)` would have. Logic enters a bounce through `Hard` alone
while AAX enters through a full reconstruct — those two paths have to agree bit-for-bit.
If a structural parameter moved during the live session, your processor reached its current
shape through the live-swap path; whether that is bit-identical to a freshly configured one
is a property of your swap code that nothing in the interface enforces.

---

## 3. `Accepted_latency` → `Reset::Latency`

Mechanically: delete the `Accepted_latency` arm from `handle_event` and move it into the
`reset` visitor as shown above. The transport changed; the contract did not.

But since this is the section a latency-negotiating adapter will be read against, here is
the whole protocol from the kernel's side — it is small, and every clause is load-bearing.

### Only three things touch your latency

| | who decides | when |
|---|---|---|
| `configure(Config)` | **you** | you set it from `(sr, params)`; the framework reads `latency_samps()` and reports it. Not a negotiation. |
| `context.propose_latency = N` | **you ask** | during `process`, and *only* in response to a live parameter change |
| `reset(Reset::Latency{n})` | **the host** | it has aligned its graph; adopt `n` immediately |

### The rule

> **The kernel proposes; it never applies.** Keep rendering at your current latency until
> handed an accepted value, then match it exactly.

So a live structural change looks like this:

1. You are rendering at `L`. A parameter change implies `L'`.
2. Set `context.propose_latency = L'` during `process`. **Keep rendering at `L`.**
3. Some time later — possibly much later — `reset(Reset::Latency{n})` arrives. *Now* you
   switch, and `latency_samps()` must equal `n` **before that call returns**. The wrapper
   asserts it, so deferring the swap to your next `process` trips the assert.

### Four things that bite

- **Restate the intention, don't toggle.** Derive the proposed value from the *parameter*,
  not by flipping off your current state. The host may sit on a proposal for a long time
  (Logic defers until playback starts), so what matters is that the outstanding proposal
  always reflects the current intention. If the user toggles back, the new proposal must
  supersede the old one rather than leave a stale value for the host to find.
- **`n` is not guaranteed to equal what you proposed.** AAX owns the value and may clamp
  it. There is no rejection path in the protocol: you match `n`, whatever it is.
- **Not proposing is the default.** The framework hands you a fresh `Dsp_context` every
  block, so `propose_latency` starts disengaged — leaving it alone *is* "no proposal".
  Re-proposing the same value every block is deduped by the wrapper and therefore harmless,
  but it is still a sign the kernel is asking rather than stating.
- **`Hard`/`Soft` must not move `latency_samps()`.** See **Traps**.

### Unsupported, by decision

- Latency derived from anything outside `(sr, params)` — e.g. a worker-loaded IR that
  changes convolution latency. Works in four formats, silently breaks on AAX.
- A different oversampling factor for offline bounce. The active factor would be selected
  by render mode, which is deliberately *not* in `Config`, so `latency_samps()` after
  `configure` would be undefined. Report a constant latency across factors.
- Sweeping a continuously-variable latency control. The dedupe only blocks repeats of the
  same value, so a knob sweep produces one host restart per distinct value. No rate limit
  exists; that is the author's problem.

---

## 4. `Render_event` → `process::Event::Any`, and the edit/process split

```cpp
process::Event::Set   // was Set_param   — PLAIN space, to the kernel
process::Event::Ramp  // was Ramp_param  — PLAIN space, to the kernel
process::Event::Any   // was Render_event
tiny::Set_param       // still exists    — KNOB space, editor / undo history
```

`Set_param` and `Event::Set` are structurally identical and deliberately distinct types.
They always carried different units; nothing enforced it until now.

**This is the step that will find bugs in your code.** Every compile error here is a place
where a knob-space value was flowing into the kernel or vice versa. Before you "fix" one by
changing the type, check which space the value is actually in:

- Editor emits, undo history stores, `Host_preset_loaded` carries → **knob** → `Set_param`.
- Anything handed to `Processor::handle` or queued for the audio thread → **plain** →
  `process::Event::Set`.
- The bridge is `Value_helper::knob_to_plain` / `host_to_plain`, at the wrapper boundary.
  In tinyplug's own migration there was **no bridging code to write** — every crossing
  already had the conversion in the same expression. If you find a crossing that does not,
  you have found a bug, not a missing bridge.

`handle_event` is also renamed to `handle`.

---

## 5. `tiny::plugin` → `tiny::process`

Your `Processor` class moves. Inside `tiny::process` the whole vocabulary resolves
unqualified — `Config`, `Reset::Any`, `Event::Any`, `Dsp_context`, `Render_mode`,
`Musical_context`, `Some_plug_processor` — so this step *deletes* qualification rather than
adding it (it removed 82 in tinyplug).

```cpp
namespace tiny::process {          // was tiny::plugin

class Processor { /* ... */ };
static_assert(Some_plug_processor<Processor>);

}
```

Anything else the processor touches that is still in `tiny::plugin` needs qualifying —
in practice that means **worker** types, since the `Worker` is shared by both sides:

```cpp
_worker.push(plugin::Tick{ /* ... */ });
auto handle_worker_reply(const plugin::Worker::Model::To_processor& r) -> void;
```

`Editor` stays in `tiny::plugin` for now. `tiny::edit` and `tiny::work` are planned; do not
pre-empt them.

---

## Traps

**1. `Hard`/`Soft` must not move `latency_samps()`.** The convergence obligation says
`Hard` leaves you where `configure(sr, current_values)` would have, and `configure` reports
a final latency — so read literally, a `Hard` arriving while a proposal is outstanding
would have you jump to the un-accepted latency. It must not. The latency contract wins:
stay where the last accepted `Reset::Latency` put you. Convergence covers everything
*except* latency-bearing structure.

**2. Render mode must not influence allocation or latency.** Parameters are structural
inputs and arrive at `configure`; render mode is an advisory per-block flag. AAX makes the
opposite look workable — it latches offline per configuration — but a kernel that sized for
offline would be honoured on AAX and silently ignored on the other four formats.

**3. Latency derived from anything outside `(sr, params)` is unsupported.** A worker-loaded
IR that changes convolution latency works in four formats and silently breaks on AAX.

**4. Ordering.** Wrappers issue `Reset::Latency` *before* `Hard`/`Soft` in a block, so a
structural swap lands before history is cleared. If your `Hard` only clears the currently
active path, that ordering is what keeps it correct — do not rely on the reverse.

---

## Verifying

Build first; the compiler does most of the work. Then the three invariants worth an actual
test:

1. **Value delivery**, for non-structural parameters only:
   `configure(sr, values)` → render **≡** `configure(sr, defaults)` → feed values →
   `reset(Soft)` → render. Bit-identical. For a *structural* parameter the two paths are
   **required** to differ, which is why the quantifier matters.
2. **Configuration finality:** after `configure(sr, {P = v})` for a structural `P`,
   `latency_samps() == L(v)` immediately, and rendering N blocks yields no proposal on any
   block.
3. **Convergence:** `configure(sr, V)` → `reset(Hard)` → render **≡**
   `configure(sr, defaults)` → set V live → renegotiate → `reset(Hard)` → render. This is
   Logic-vs-AAX bounce entry, and it is what `Hard`'s obligation buys.

`tests/allocation/` should cover `reset` in all three forms staying allocation-free.
