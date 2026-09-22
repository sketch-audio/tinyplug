# Plan: Block output — processor → editor frame transport

> Status: **implemented** (2026-09-22). Reference: [examples/block_demo](../examples/block_demo/),
> porting notes in [MIGRATION.md](../MIGRATION.md) "Blocks". Where the build departs from
> this document:
>
> - **In-process and VST3 use `Data_port`**, not the two-slot `seq` store. The seq store's
>   copy is a formal data race whenever the producer laps the reader; `Data_port` is a
>   proper handoff and TSan-clean. Cost: one extra frame copy per publish, because the
>   processor fills a staging frame in the `Publisher`. The seq store survives **only in
>   AAX** ([block_store.hpp](../wrappers/aax/source/block_store.hpp)), where the reader is
>   `ReadPortDirect` and cannot take part in a handoff.
> - **One mailbox type, no `Transport` split.** The VST3 controller's `notify` may run on
>   the relay's thread, so the controller side needs the lock-free mailbox too.
> - **One VST3 message ID**, `tiny/blocks`, with the address in the tag. The relay runs
>   at 60 Hz.
> - **Everything lives in `tiny_blocks.hpp`** (plus `data_port.hpp`), following the
>   meters layout. `blocks::None` rather than `No_blocks`. The editor gets a `View`
>   onto wrapper-owned `Frames`; `Processor_state` moved into the interface to hold it.
> - Open decisions: no `Float_frame`, no missed count on `fresh`, and no frame-size
>   `static_assert` — neither `IMessage::setBinary` nor `AddPrivateData` documents a
>   ceiling below `int32_t`.

## What changed in this revision

The first draft was written before the meter pipeline shipped and before the VST3
wrapper's messaging layer settled. Five amendments, four of them removals:

| Was | Now | Why |
|---|---|---|
| `std::span<float>` payloads | author's own trivially-copyable frame type | a frame is bins *plus* its fft size, hop, rate — hand-packing that into float slots is the marshalling the framework exists to delete |
| `snapshot` + `stream` policies | **snapshot only** | a queue is the wrong shape for coalescing data; same argument that deleted `Meter_queue` |
| `Block_channel` (triple buffer + SPSC ring) | `Block_store` (two slots + one seq word) | must be readable by remote memcpy, which a pointer-swap triple buffer is not |
| VST3 `IDataExchangeHandler` | `Relay` + `IMessage` | **already tried and rejected in this codebase** — see below |
| AAX `Ring_kind::Block_chunk` on the return ring | dedicated private-data field | 4 KB frames per block through a framed ring is the wrong transport for latest-wins |

The VST3 one is the important correction.
[messaging.hpp:66-73](../wrappers/vst3/source/messaging.hpp#L66-L73) records why
`Outbound_message_shuttle` exists: *"Replaces our (failed) attempt to use the SDK's
DataExchangeHandler whose fallback path was unreliable in Bitwig and Live 11."* The
original §5.2 recommended the thing that was already thrown out.

---

## Context

tinyplug transports **scalars** between the two sides: parameters in, meters out.
Visualization needs **frames** out — a fixed-size struct produced by the audio
thread (or a background job) and drawn by the immediate-mode editor.

| | Editor → Processor | Processor → Editor |
|---|---|---|
| **Scalar** | `params::Model` | `meters::Model` |
| **Frame / buffer** | `Buffer` ([buffer-system.md](buffer-system.md)) | **`blocks::Model` (this plan)** |

Uses: spectrum analyzers, oscilloscopes, envelope and transfer curves, and the
waveform overview every buffer-system plug-in draws. Overviews are deliberately a
block rather than part of the buffer system: the buffer system owns content,
transport and persistence; the editor's *picture* of it is an ordinary block. The
two compose, and the overview's producer is the `prepare_buffer` job rather than
the audio thread — which this design permits, one producer per address at a time.

Blocks carry **no permanence surface at all**. No identifier, no persistence, no
append-only rule on addresses — a block is transient by definition. That is what
makes this model much cheaper than `state::Model`, and it should be said out loud
because every other model in the framework has at least one.

---

## 1. Declarative model (`libs/tinyplug_core/include/tinyplug_core/tiny_blocks.hpp`)

### The concept does not mandate an enum

Following the relaxation agreed for all models: the framework needs a compile-time
count and stable addresses `0..<num_blocks`. It does not need to know how the
author names them.

```cpp
namespace tiny::blocks {

struct Spec {
    std::size_t kind{};   // index into the model's Types
    auto operator==(const Spec&) const -> bool = default;
};

template<typename T>
concept Model = requires {
    typename T::Types;                                       // variant of frame types
    typename std::integral_constant<std::uint32_t, T::num_blocks>;
    { T::make_spec(std::uint32_t{}) } -> std::same_as<Spec>;
};

} // namespace tiny::blocks
```

`make_spec` **must be `constexpr` and `Spec` must be a literal type**, because
`kind` is used as a template argument to resolve the frame type at each call site.
This is a real asymmetry with `params::Model`, whose `Spec` holds a `std::string
identifier` and can never be constexpr — params needs no typed access, so it is not
a conflict, but it is the one place "every model reads the same" has an asterisk.

Author view (`examples/<plug>/source/models/blocks.hpp` — core-only, no interface
types):

```cpp
struct Spectrum_frame {
    float bins[1024]{};
    uint32_t used{};
    float rate{};
    float hop{};
};

struct Scope_frame {
    float samples[512]{};
    uint32_t trigger{};
    float samples_per_pixel{};
};

struct Blocks {
    using Types = std::variant<Spectrum_frame, Scope_frame>;

    // The enum is the author's, for the author's exhaustiveness check. No sentinel.
    enum class Address : uint32_t { Spectrum, Scope };

    static constexpr uint32_t num_blocks = 2;

    static constexpr auto make_spec(uint32_t i) -> blocks::Spec
    {
        switch (static_cast<Address>(i)) {
            case Address::Spectrum: return {blocks::kind_of<Spectrum_frame, Types>};
            case Address::Scope:    return {blocks::kind_of<Scope_frame, Types>};
        }
        return {};
    }
};
static_assert(blocks::Model<Blocks>);
```

Casting back at the top of the switch keeps `-Wswitch` — verified: adding an
enumerator without a case still produces *"enumeration value 'Envelope' not handled
in switch"*. Dropping the sentinel enumerator also drops the
`case Num_blocks: default: return {};` stanza that is pure ceremony in every model
today.

The cost is that `num_blocks` can drift from the enum, silently in both directions.
Authors who want the old ergonomics keep a sentinel and write
`static constexpr uint32_t num_blocks = enum_raw(Address::Num_blocks);`. A
concept-detected static_assert holds them to it when the sentinel exists. Authors
who want a *computed* count — `4 * num_bands`, sixteen slots named in a loop — can
now have one, which an enum-mandated concept forecloses entirely.

### Frame type requirements

- `std::is_trivially_copyable_v<Frame>` — hard. Every transport below is a memcpy.
- Fixed `sizeof(Frame)`, bounded by a per-format transport limit.
- No pointers, no references, no indices into anything the other side cannot see.

These are the constraints the worker channel already lives under
([messaging.hpp](../wrappers/vst3/source/messaging.hpp) `send_pod` / `send_variant`),
and they have held up.

The framework never looks inside a frame, so it cannot offer generic drawing. That
capability is recovered by *shipping* a blessed type — `blocks::Float_frame<N>`,
carrying `used` and a small header, with a `valid()` returning
`std::span<const float>` — rather than by making floats the only option. Mechanism
opaque, convenience provided.

Default empty model: `blocks::No_blocks` with `num_blocks = 0` and a `make_spec`
returning `{}`. A real zero-entry model, not a `monostate` — per
[model-layer.md](model-layer.md) M3's finding. Three lines, which is the direct
benefit of the relaxed concept.

---

## 2. The transport primitive: `Block_store`

One structure, three readers. The hard constraint comes from AAX: the consumer
reaches the producer's memory only through
`AAX_IPrivateDataAccess::ReadPortDirect(field, offset, size, out)` — a memcpy of a
byte range, with no shared object identity and no way to hold a pointer. That rules
out a pointer-swap triple buffer. It does not rule out the following:

```cpp
// Standard layout with published offsets, following Byte_ring's discipline.
template<typename Frame>
struct Block_store {
    std::atomic<uint64_t> seq{};   // 0 = nothing published. Slot = (seq - 1) & 1.
    Frame slots[2]{};

    static constexpr auto offset_seq = uint32_t{0};
    static constexpr auto offset_slots = uint32_t{...};
};
```

**Producer**: fill `slots[seq & 1]` — the back slot — then `seq.fetch_add(1, release)`.
**Consumer**: read `seq`, copy the indicated slot, read `seq` again, accept when
unchanged.

### Why two slots and not a seqlock

The original draft proposed a seqlock (`[seq][payload]`, odd while writing). It
works, and it has one failure mode this design does not: an author who calls
`write()` and then early-returns without `publish()` leaves `seq` odd forever and
wedges the channel permanently. With two slots there is no in-progress state — an
abandoned write leaves the back slot dirty, the reader keeps seeing the last good
frame, and the next publish overwrites the mess. The retry window is identical.

The producer is zero-copy: the author fills the staging slot in place, so there is
exactly one memcpy on the audio thread and the publish is a single store. The
publish *rate* is author-controlled — an FFT publishes per hop, not per block —
which is why this stays cheap at 32-sample buffers.

### Why not a ring

Same argument that deleted `Meter_queue`, recorded at
[meter_mailbox.hpp:24-32](../libs/tinyplug/include/tinyplug/meter_mailbox.hpp#L24-L32):
a transport that preserves every intermediate value is spending capacity on data the
editor will never draw, and when it runs out it drops the newest. A 4 KB frame per
block through a framed ring drained at 33 Hz needs either an enormous ring or
constant refusals. Blocks coalesce. A slot array has no capacity to run out.

---

## 3. The two halves: `Publisher` and `Mailbox`

Mirroring `meters::Publisher` / `meters::Mailbox`, which is the shape that ended five
hand-rolled wrapper loops.

**`blocks::Publisher<Model>`** — processor-adjacent. Owns the stores, hands out typed
write access, and transmits. Simpler than the meter publisher: nothing to deduplicate,
no falling-edge problem, no per-policy branch. The one thing it keeps is the
**offline-bounce gate** — the same `offline || !renders_audio` condition the meter
publishers use ([plugin.cpp:293](../wrappers/clap/source/plugin.cpp#L293)). A bounce
has no editor to draw into and every "tell the other side" mechanism costs the render.

**`blocks::Mailbox<Model>`** — editor-adjacent. One retained slot per address.

```cpp
template<Model M> class Mailbox {
public:
    template<auto A> auto latest() const -> const Frame<A>&;   // always answers
    template<auto A> auto fresh() const -> const Frame<A>*;    // null if nothing new
};
```

`latest` is for a picture that should stay on screen; `fresh` is for a scope that
should only redraw on new data. The distinction is free here because there is a seq
to diff — it is the same discipline `Peak` needed (*"nothing arrived" is not the same
as "a measurement of zero"*), without needing a post counter to reconstruct it.

There is **no resync hole and no `discard()`**: latest-wins with a retained slot means
an editor opening at any moment simply reads what is there. AAX refreshes on the next
33 Hz wakeup, VST3 on the next publish, in-process immediately.

### Typed access without the framework naming the enum

Verified by compilation (`-Wall -Wextra`, clang, C++20):

```cpp
template<auto A>
inline constexpr uint32_t index_of = static_cast<uint32_t>(A);

template<Model M> struct Publisher {
    template<auto A>
    using Frame = std::variant_alternative_t<M::make_spec(index_of<A>).kind, typename M::Types>;

    template<auto A> auto write() -> Frame<A>&
    {
        static_assert(index_of<A> < M::num_blocks, "block address out of range");
        return ...;
    }
    template<auto A> auto publish() -> void;
};
```

`pub.write<Address::Spectrum>()` returns `Spectrum_frame&` with no `.template`
disambiguator at the call site, and `index_of` accepts a plain integer too.

---

## 4. Processor API

`Dsp_context` gains a model-aware member. This is the line that requires M5.

```cpp
struct Dsp_context {
    // ...existing...
    blocks::Writer<models::Blocks> blocks{};   // NEW
};
```

```cpp
auto process(Dsp_context& ctx) -> void
{
    // ... dsp ...
    if (_fft.ready()) {
        auto& frame = ctx.blocks.write<Address::Spectrum>();
        _fft.fill(frame.bins);
        frame.used = _fft.size();
        frame.rate = _rate;
        ctx.blocks.publish<Address::Spectrum>();
    }
}
```

`Writer` is a non-owning view onto the wrapper's `Publisher`, so the processor never
sees transport. Unlike the original draft it is **not** a pair of `std::function`s —
that erasure existed to keep the processor decoupled from a transport it could not
name, and after M5 it can. A `std::function` call per frame on the audio thread was
never desirable; now it is also unnecessary.

Off-thread producers (the buffer system's overview job) use the same `Writer`. The
store tolerates any single producer; what it does not tolerate is two at once for one
address. Document it; it never happens in practice because a given slot has one
author.

---

## 5. Editor API

`Processor_state` ([tiny_utils.hpp:14](../libs/tinyplug/include/tinyplug/tiny_utils.hpp#L14))
gains a blocks view; `Ui_receiver`
([tiny_events.hpp](../libs/tinyplug/include/tinyplug/tiny_events.hpp)) gains the drain
hook beside `read_meters`; `run_frame`
([tiny_view.hpp:321](../libs/tinyplug/include/tinyplug/tiny_view.hpp#L321)) drains
before `on_gui_draw`, in the same slot meters already occupy.

```cpp
auto on_gui_draw(Plugin_state& state) -> void
{
    const auto& blocks = state.processor_state.blocks;

    if (const auto* f = blocks.fresh<Address::Spectrum>()) push_column(*f);
    draw_spectrogram();

    draw_scope(blocks.latest<Address::Scope>());
}
```

One memcpy per **changed** block per frame — 4 KB at 60 Hz is 240 KB/s for a
1024-bin spectrum, and only while it is moving.

---

## 6. Per-format wiring

### 6.1 In-process (CLAP, AUv2, AUv3)

Publisher and mailbox share the store directly. The wrapper holds a
`blocks::Publisher<models::Blocks>` beside its `meters::Publisher`
([plugin.hpp:170](../wrappers/clap/source/plugin.hpp#L170)) and a
`blocks::Mailbox` beside its `meters::Mailbox`
([plugin.hpp:243](../wrappers/clap/source/plugin.hpp#L243)). CLAP is the reference
implementation; AUv2 and AUv3 are the same pattern.

### 6.2 VST3 — `Relay` + `IMessage`

Not `IDataExchangeHandler`, for the reason recorded at
[messaging.hpp:66-73](../wrappers/vst3/source/messaging.hpp#L66-L73). Not the
`Outbound_message_shuttle` either: it is entirely `TINY_HAS_WORKER`-gated
([audio_effect.hpp:165-181](../wrappers/vst3/source/audio_effect.hpp#L165-L181)) and
paced by `User_worker::Model::update_period`, so a plug-in with a spectrum analyzer
and no worker has no outbound thread at all.

Use `Relay` ([relay.hpp:38-43](../libs/tinyplug/include/tinyplug/relay.hpp#L38-L43)).
Its contract is *"one release store, no allocation, no lock. Coalescing is the point —
N posts inside one interval cost one callback"*, which **is** latest-wins:

- `Audio_effect::process` fills the store and calls `_block_relay.post()`.
- The relay fires on an OS timer (~0.016 s for visualization; the latency relay runs
  at 0.05) and sends one `IMessage` per changed address carrying the current slot.
- `Vst3_controller::notify` routes it through `vst3::Message_router` under a
  `tiny/blocks/<addr>` ID and posts into the controller-side mailbox.

`sendMessage` from a relay callback is already proven in this wrapper — the latency
path does exactly this ([audio_effect.cpp:205-214](../wrappers/vst3/source/audio_effect.cpp#L205-L214)).
Scope the relay to `setActive` the same way.

Meters are unaffected and stay on output parameter changes. That decision was
re-litigated and confirmed in [meter-pipeline.md](meter-pipeline.md); blocks are a
different shape (no host timing to preserve, too large for a parameter) and get their
own wire.

### 6.3 AAX — Direct Data

The format the SDK names this use case for outright: Direct Data exists so "the result
of computing the audio spectrum or pitch data in the algorithm can be delivered to the
host to display on-screen", and `ReadPortDirect` is a memcpy of a byte range.

| Piece | Where |
|---|---|
| `Writer::write<A>()` target | `Block_store<Frame>` in a **dedicated private-data field** |
| `publish<A>()` | `seq.fetch_add(1, release)` |
| transport | `ReadPortDirect` on the Direct Data timer, ~33 Hz |
| consumer-side mailbox | on `Parameters`, beside the meter mailbox |
| `Ui_receiver` hook | reads the data-model-side mailbox — identical to §5 |

Concretely, on top of what [wrappers/aax/source/](../wrappers/aax/source/) has:

- **A new pointer slot in `Alg_context`**, plus its `AAX_FIELD_INDEX` and an
  `AddPrivateData` in [describe.cpp](../wrappers/aax/source/describe.cpp). It cannot
  live inside `Alg_state`: that type holds `process::Processor` and is not
  standard-layout, so offset arithmetic cannot reach into it. CLAUDE.md's rule that
  *every* pointer slot must be registered applies.
- **`seq == 0` means "nothing published", not "a frame of zeros".** Private data is
  wiped at every reset (`AAX_ePrivateDataOptions_KeepOnReset` is declared but not
  implemented), so after a reset the store returns to zero while the data-model
  mailbox is still holding a frame. The reader must hold. This is precisely the
  `latency_seq` trap already documented on `Runtime_packet`
  ([alg_context.hpp:85-90](../wrappers/aax/source/alg_context.hpp#L85-L90)) — same
  mechanism, same fix, and it is the one AAX-specific hazard in this design.
- **The return ring is untouched.** No `Ring_kind::Block_chunk`, no change to
  `return_ring_bytes`. Blocks do not go through the ring at all, which is most of what
  §5.3 of the first draft was.
- **Sizing.** A 1024-bin spectrum is ~4 KB; at 33 Hz that is ~135 KB/s of memcpy per
  instance. Two slots doubles the staging footprint to 8 KB per address, which is
  nothing.

**Cost, unchanged from the first draft and worth restating:** the Direct Data wakeup is
~30 ms and explicitly not guaranteed to be regular, so AAX tops out near 33 fps where
CLAP and AU read at frame rate. Acceptable for visualization.

### 6.4 Compatibility matrix

| | VST3 | CLAP | AUv2 | AUv3 | AAX |
|---|---|---|---|---|---|
| Blocks | `Relay` + `IMessage` | shared store | shared store | shared store | Direct Data, ~33 Hz |

---

## 7. Why snapshot only

The `stream` / FIFO policy is cut. Removing it also removes `queue_depth`, `drain()`,
the AAX ring work, the VST3 ordering question, and open decision 3 of the first draft.

Latest-wins was tested against the obvious candidates and survives all of them. A
scope draws one triggered frame per refresh — frames it would drop are frames it would
never draw. Buffer-system overviews are latest-wins by nature. A block-shaped `Trig`
event is speculative and nothing needs it.

**The one case it genuinely cannot serve is a scrolling history** — a spectrogram or
waterfall, where each frame is a *column* and the picture is the accumulation. At an
86 Hz hop against a 60 Hz draw, latest-wins drops ~30% of columns and the time axis
beats against the frame rate: visibly wrong, not merely lossy.

The answer is not a FIFO. A scrolling history is a different data shape — not "the
latest value" but "the producer's current window" — so **the producer owns the history
ring and publishes a snapshot of the last N columns.** That is still latest-wins, it is
*more* robust (a UI stall costs nothing while it is shorter than N columns, where a
FIFO drops a burst and tears the scroll), and the picture is always self-consistent.

That escape hatch fails only when the history is long *and* the frames are large — a
30-second, 512-bin spectrogram is ~5 MB, too big to republish, so you would send only
new columns, which is a FIFO. Real, but not on the roadmap, and it is the case AAX's
bursty 33 Hz wakeup serves worst anyway. **Revisit only when something needs it**; the
model concept and `Spec` both have room for a policy field to come back.

---

## 8. Implementation sequence

Everything below assumes [model-layer.md](model-layer.md) steps 5 and 7 have landed.

1. `tiny_blocks.hpp` in core — `Spec`, `Model`, `kind_of`, `Infos`, `Block_store`,
   `No_blocks`, `Float_frame<N>`.
2. `blocks::Publisher` / `blocks::Mailbox` / `blocks::Writer`, with standalone tests
   before any wrapper touches them (the meter pipeline's tests found two bugs that
   reasoning did not — promote these into the repo suite from the start, not after).
3. `models/blocks.hpp` discovery in `configure_models`; `User_blocks` in the generated
   `tiny_models.hpp`; confirm every existing example compiles with `No_blocks`.
4. `Dsp_context::blocks`; `Processor_state::blocks`; `Ui_receiver` hook; `run_frame`
   drain.
5. CLAP wrapper — the reference. Then AUv2, AUv3.
6. VST3 — relay, message IDs, controller-side mailbox.
7. AAX — private-data field, Describe registration, Direct Data read, `seq == 0`
   handling.
8. `examples/block_demo` — a spectrum (snapshot redrawn every frame) and a scope
   (`fresh` only), with a README listing what each proves per host, in the style of
   `meter_demo`.

---

## 9. Open decisions

1. **Where `Block_store` lives for the in-process formats.** Inside the `Publisher`
   is simplest; AAX needs it in a separate private-data allocation regardless, so the
   publisher must be able to *point at* a store it does not own. Probably: publisher
   holds `std::span`-like access, wrapper owns the storage.
2. **Frame size ceiling.** `IMessage::setBinary` and `ReadPortDirect` both have
   practical limits worth pinning down before an author hits one. A `static_assert` on
   `sizeof(Frame)` against a per-format constant is cheap insurance.
3. **`Float_frame<N>` — ship it in v1 or wait?** It is the generic-drawing escape
   hatch, but it is also the thing authors will reach for without thinking, which
   undercuts the typed design. Leaning: ship it, document it as the fallback.
4. **Does `fresh<A>()` need a "how many were missed" count**, the way `Trig` needs one?
   For a spectrogram it would let the editor interpolate rather than glitch. Cheap (the
   seq difference is already there) but speculative.

---

## Verification

- Every existing example compiles unchanged against `No_blocks`.
- Publisher/mailbox unit tests, in the repo suite, covering: a reader running slower
  than the producer by 1, 5, 20 and 64 publishes; an abandoned `write` with no
  `publish`; a store reset to `seq == 0` under a live mailbox; `fresh` returning null
  on a frame with no publish.
- TSan across the store with an audio-thread producer and a UI-thread reader, plus the
  off-thread (prepare-job) producer case.
- `block_demo` in all five hosts; AAX specifically for the reset case, since that is
  where `seq == 0` bites.
- AAX validator, Steinberg validator, clap-validator all clean.

---

## Key files

| File | Change |
|---|---|
| `libs/tinyplug_core/include/tinyplug_core/tiny_blocks.hpp` | **New** — model, `Block_store`, `Infos`, `No_blocks` |
| `libs/tinyplug_core/include/tinyplug_core/block_publisher.hpp` | **New** |
| `libs/tinyplug_core/include/tinyplug_core/block_mailbox.hpp` | **New** |
| `libs/tinyplug/include/tinyplug/tiny_processor.hpp` | `Dsp_context::blocks` |
| `libs/tinyplug/include/tinyplug/tiny_events.hpp` | `Ui_receiver` drain hook |
| `libs/tinyplug/include/tinyplug/tiny_utils.hpp` | `Processor_state::blocks` |
| `libs/tinyplug/include/tinyplug/tiny_view.hpp` | drain in `run_frame` |
| `cmake/` (`configure_models`) | discover `models/blocks.hpp` |
| `wrappers/clap/source/plugin.{hpp,cpp}` | publisher + mailbox (reference) |
| `wrappers/auv2/source/effect.{hpp,cpp}` | same pattern |
| `wrappers/auv3/source/extension/audio_unit.mm` | same pattern |
| `wrappers/vst3/source/audio_effect.{hpp,cpp}` | store + block relay |
| `wrappers/vst3/source/controller.{hpp,cpp}` | message handler + mailbox |
| `wrappers/aax/source/alg_context.hpp` | `Block_store` field + field index |
| `wrappers/aax/source/describe.cpp` | `AddPrivateData` registration |
| `wrappers/aax/source/direct_data.cpp` | `ReadPortDirect` per block, `seq == 0` guard |
| `wrappers/aax/source/parameters.{hpp,cpp}` | consumer-side mailbox |
| `examples/block_demo/` | **New** |
