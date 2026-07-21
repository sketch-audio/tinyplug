# Plan: AAX two-component wrapper (algorithm + data model)

> Status: **implemented** on branch `aax-two-component` (phases 1–5 of §14). The AAX
> wrapper now uses AAX's native two-component design — a decoupled real-time algorithm
> callback plus a host-side data model — so AAX matches the value-semantics discipline
> VST3 already enforces. All six demo plug-ins build for all four desktop formats.
> **Not yet run in Pro Tools or the AAX validator** — see §15.
>
> The arbitrary-parameter problem that motivated the forum question is solved without
> code generation. Two genuine costs remain (§8 return-channel latency, §10 buffer
> system); both have concrete answers.

## Implementation status

| Phase | State |
|---|---|
| 1. Scaffolding (`alg_context.hpp`, real component descriptor) | done |
| 2. Parameters + audio (coefficient segments, `Host_bypass` moved, `InstanceInit`) | done |
| 3. Delete the vendored monolith | done — `monolith.hpp`/`.cpp` removed |
| 4. Direct Data return channel (meters, worker, latency) | done |
| 5. Latency handshake over the new channel | done |
| 6. Validator + Pro Tools host matrix | **outstanding** |
| 7. Plan follow-ups (block-output, buffer-system, midi-support) | block-output + buffer-system done |

Two decisions changed during implementation, both simplifications:

- **One coefficient array, all `Buffered`** — not the `coefs_auto[]` / `coefs_fast[]`
  split by `params::Policy` described in §3. The split needed either a runtime
  partition the algorithm would have to re-derive, or two worst-case-sized arrays
  (double the ports). The benefit it buys — avoiding a buffer split when an
  editor-only parameter changes — is a transient on each GUI edit, not a steady-state
  cost. Recorded as a future optimisation, not a regression: today's wrapper has no
  such distinction either.
- **Master bypass packs as a pseudo-parameter** at `bypass_address == num_params`
  rather than getting its own port, so `Host_bypass` moved into `Alg_state` and the
  data model stopped tracking bypass state entirely.

---

## Contents

1. [Why bother](#1-why-bother)
2. [What the two-component model actually is](#2-what-the-two-component-model-actually-is)
3. [Arbitrary parameters without code generation](#3-arbitrary-parameters-without-code-generation)
4. [The proposed algorithm context](#4-the-proposed-algorithm-context)
5. [Where the processor lives](#5-where-the-processor-lives-private-data--instanceinit)
6. [Responsibility migration table](#6-responsibility-migration-table)
7. [Parameter flow in detail](#7-parameter-flow-in-detail)
8. [The return channel: Direct Data](#8-the-return-channel-direct-data)
9. [Feasibility: block-output](#9-feasibility-block-outputmd)
10. [Feasibility: buffer-system](#10-feasibility-buffer-systemmd)
11. [Feasibility: midi-support](#11-feasibility-midi-supportmd)
12. [What we lose, and residual risks](#12-what-we-lose-and-residual-risks)
13. [Three designs, and the recommendation](#13-three-designs-and-the-recommendation)
14. [Implementation sequence](#14-implementation-sequence)
15. [Open questions to verify empirically](#15-open-questions-to-verify-empirically)
16. [Key files](#16-key-files)
17. [Source references](#17-source-references)

---

## 1. Why bother

The current wrapper ([wrappers/aax/source/parameters.hpp](../wrappers/aax/source/parameters.hpp))
extends a **vendored, hand-edited copy** of `AAX_CMonolithicParameters`
([monolith.hpp](../wrappers/aax/source/monolith.hpp) /
[monolith.cpp](../wrappers/aax/source/monolith.cpp), ~760 lines). Everything —
DSP kernel, editor, worker, meter queue, bypass, latency state — lives on one
object, and the algorithm reaches it through a `this` pointer smuggled into a
private data block (`AAX_SInstrumentPrivateData::mMonolithicParametersPtr`, set in
`ResetFieldData`). It is the one wrapper where the framework's own
processor/editor separation is *not* backed by any structural enforcement.

Five concrete wins from going decoupled:

1. **Delete the vendored monolith.** `monolith.hpp`/`monolith.cpp` exist only to
   supply the deferred-parameter-synchronization machinery (`SParamValList`,
   `TNumberedStateListQueue`, `GetUpdatesForState`, `AddSynchronizedParameter`).
   In the decoupled model **the host does all of that for us** — it timestamps
   packets and splits render buffers to deliver them (§2). The vendored file, and
   the "don't replace this with the SDK version without understanding why" warning
   in [CLAUDE.md](../CLAUDE.md), both go away.

2. **Better automation timing, for free.** Today `RenderAudio` applies every
   synchronized parameter value at **offset 0 of the buffer**
   ([parameters.cpp:583-614](../wrappers/aax/source/parameters.cpp#L583-L614)) —
   buffer-granular. Decoupled, the host divides Native buffers down to
   `AAX_eAudioBufferLengthNative_Min` = **32 samples** to land a packet as close as
   possible to its automation breakpoint, *and guarantees the chosen sample is
   deterministic across playback passes*. That is strictly better than what we do
   now, at zero implementation cost.

3. **Value semantics become structural, not aspirational.** The algorithm's only
   window on the world is its context struct. There is no `Parameters*` to
   dereference. This is the same discipline `kDistributable` forces on VST3.

4. **[block-output.md](block-output.md) gets a first-class home.** The Direct Data
   interface exists specifically for "the result of computing the audio spectrum or
   pitch data in the algorithm ... delivered to the host to display on-screen", and
   its API (`ReadPortDirect(field, offset, size, outBuffer)`) is a **memcpy across
   the boundary** — value semantics by construction (§9).

5. **HDX/TI stops being structurally impossible.** `AAX_CMonolithicParameters`
   "precludes the use of ... Effects in distributed-processing formats such as AAX
   DSP". We would not ship a TI build in phase 1, but the door stops being welded
   shut.

The counter-argument — "it works today" — is real, and §12/§13 weigh it.

---

## 2. What the two-component model actually is

Reference: [AAX_CommonInterface_Algorithm.doxygen](../../tiny_deps/third_party/aax-sdk/Documentation/Doxygen/dox/AAX_CommonInterface_Algorithm.doxygen),
[AAX_CommonInterface_Describe.doxygen](../../tiny_deps/third_party/aax-sdk/Documentation/Doxygen/dox/AAX_CommonInterface_Describe.doxygen),
example [DemoDelay](../../tiny_deps/third_party/aax-sdk/ExamplePlugIns/DemoDelay/Source/).

- The **algorithm** is a C callback over a batch of instances:

  ```cpp
  void AAX_CALLBACK Proc(My_context* const inBegin[], const void* inEnd);
  ```

- Its entire world is the **context struct** — a struct of *pointers only*. The
  host owns the memory and repopulates every field before each call.

- Field identity is **`AAX_FIELD_INDEX(Ctx, member) == offsetof(Ctx, member) / sizeof(void*)`**
  ([AAX.h:296](../../tiny_deps/third_party/aax-sdk/Interfaces/AAX.h#L296)) —
  i.e. *a pointer-slot index*. This is the key fact for §3.

- Field kinds we care about, registered in Describe on `AAX_IComponentDescriptor`:

  | Call | Gives the algorithm |
  |---|---|
  | `AddAudioIn` / `AddAudioOut` | `float**` channel arrays |
  | `AddAudioBufferLength` | `int32_t*` frames this call |
  | `AddSampleRate` | `float*` sample rate |
  | `AddSideChainIn` | `int32_t*` index into the input array |
  | `AddDataInPort(idx, size, type)` | read-only packet destination |
  | `AddPrivateData(idx, size, opts)` | persistent, never-relocated block |
  | `AddMeters(idx, ids, count)` | `float**` meter taps |
  | `AddMIDINode(idx, type, name, mask)` | MIDI in/out/global/**transport** |
  | `AddClock` | `AAX_CTimestamp*` |

- **Every pointer slot in the struct must be registered.** The vendored monolith
  registers dummy `AddPrivateData(idx, sizeof(float))` fillers for unused slots
  "to avoid context corruption" — we must do the same discipline.

- **Data in** is `AAX_IController::PostPacket(fieldIndex, payload, size)`, called
  from `GenerateCoefficients()` on the data model. Two port types
  ([AAX_Enums.h](../../tiny_deps/third_party/aax-sdk/Interfaces/AAX_Enums.h),
  `AAX_EDataInPortType`):

  | | Semantics | Use for |
  |---|---|---|
  | `Buffered` (default) | timestamp-synced; *never* updated mid-callback; host splits buffers to land it at the breakpoint | automatable params |
  | `Unbuffered` | latest post delivered ASAP; may change *during* algorithm execution | non-automatable / editor-only params, host-state flags |

  Note the bug-list entry: `PostPacket()` during `AAX_IEffectParameters::TimerWakeup()`
  is broken **for unbuffered ports only** (PTSW-187216, will-not-fix). Keep posting
  inside `GenerateCoefficients()` and this never bites.

- **Data out** is either host-managed meters (`AddMeters` → `AAX_IController::GetCurrentMeterValue`)
  or the **Direct Data** module (§8).

- **Initialization order** (documented, and load-bearing for §5):

  1. data model initialized (`EffectInit`)
  2. `ResetFieldData()` per private data block (host-side)
  3. initial `GenerateCoefficients()`; packets dispatched
  4. **all packets delivered; initial context state set**
  5. optional `AAX_CInstanceInitProc` called *with the default context*, in the
     algorithm's memory space, with up to 5 seconds
  6. processing begins

---

## 3. Arbitrary parameters without code generation

This is the question posed on the developer forum, and Rob Majors' answer is the
right one:

> My recommendation is to divide the parameter list into segments. The minimum data
> transfer size to the DSP chips on HDX cards is 128 bytes... The AAX Describe APIs
> only require an **offset** into the coefficient data struct... **You don't need a
> name for the offset** within the struct as long as both your Describe and
> algorithm processing logic can reliably agree on the offset.

Corroborated by the SDK's own HDX guide
([AAX_TI_Guide.doxygen](../../tiny_deps/third_party/aax-sdk/Documentation/Doxygen/dox/AAX_TI_Guide.doxygen)):

- 128-byte minimum host→DSP transfer; sub-128-byte packets waste bandwidth.
- Each **buffered** port costs ~5 cycles per render callback (Pro Tools 10.2) —
  "we strongly recommend that plug-ins use **consolidated coefficient packets**".
- HDX caps at **164 buffered data ports per DSP** (PTSW-158119).

### The trick: an array of pointers in the context struct

`AAX_FIELD_INDEX` is `offsetof / sizeof(void*)`. A C array of pointers therefore
occupies **contiguous, arithmetically-derivable field indices** — no macro, no
named fields, no code generation:

```cpp
static constexpr auto params_per_segment = 15;   // 15 doubles + seq = 128 bytes
static constexpr auto num_segments =
    (Infos<models::Params>::num_params + params_per_segment - 1) / params_per_segment;

struct Alg_context {
    // ... fixed slots ...
    const Coef_segment* coefs[num_segments];      // <- N contiguous slots
    // ... more fixed slots ...
};

static constexpr auto coef_field(uint32_t seg) -> AAX_CFieldIndex {
    return AAX_FIELD_INDEX(Alg_context, coefs) + static_cast<AAX_CFieldIndex>(seg);
}
```

`num_segments` is a compile-time constant derived from `models::Params`, so the
struct is sized per plug-in at compile time. Describe and the algorithm agree by
construction because both call `coef_field()`.

### Segment payload

```cpp
#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_ALG
#include AAX_ALIGN_FILE_END

struct Coef_segment {
    uint64_t seq;                        // bumped by the data model on every regen
    double   value[params_per_segment];  // PLAIN-space values
};
static_assert(sizeof(Coef_segment) == 128);
```

Design notes:

- **Plain space, doubles.** The data model does *all* conversion
  (`Value_helper::host_to_plain` / `knob_to_plain`) — off the real-time thread,
  which is a small win over today, where `RenderAudio` does the conversion inline.
  `double` rather than `float` because plain values can be large integers (sample
  counts) where float32's 24-bit mantissa is not enough.
- **`seq` lets the algorithm skip untouched segments** in O(1).
- **Shadow-compare finds which params changed.** The algorithm keeps a
  `double shadow[num_params]` in private data. Per render callback, per segment
  whose `seq` differs from the shadow's, compare the ≤15 doubles and emit
  `Set_param{addr, value}` into `Processor::handle_event` for each difference. For
  a 200-parameter plug-in that is at most 200 `double` compares per callback,
  invisible next to the DSP.
- **Alternative considered and rejected:** a dirty bitmask in the packet. It saves
  the compare but costs payload bytes and forces the data model to reason about
  which updates the algorithm has already consumed — a distributed-state problem we
  do not need. The shadow is stateless from the data model's point of view.

### Port count and traffic

| Params | Segments (15/seg) | Buffered ports | Bytes per full refresh |
|---|---|---|---|
| 8 | 1 | 1 | 128 B |
| 60 | 4 | 4 | 512 B |
| 200 | 14 | 14 | 1.75 KB |
| 1000 | 67 | 67 | 8.5 KB |

Comfortably under the HDX 164-port cap up to ~2400 parameters, and far under it in
practice. Native has no documented port limit.

### Does chunk load explode into O(N²) posts?

No. `GenerateCoefficients()` is called by the host after `UpdateParameterNormalizedValue()`,
and posts only the segments dirtied **since the last call**. A `SetChunk` that
touches N parameters therefore produces O(N) posts of 128 B each (~25 KB for 200
params), not O(N²/15). This is the same order as today's one-packet-per-parameter
behaviour in a conventional decoupled plug-in.

### Automation correctness across a shared segment

Two params in the same segment automated at different times is *not* a problem.
Each post is a **full snapshot of the segment taken at the automation timestamp**,
and the host delivers posts in timestamp order. Param A's breakpoint at t=1000
posts `{A_new, B_old}`; param B's at t=1500 posts `{A_new, B_new}`. Both are
correct at their respective render positions.

### Policy → port type

| `params::Policy` | Port type | Rationale |
|---|---|---|
| `Automation` | `Buffered` | needs timestamp sync; host may split buffers |
| `Control`, `Hidden`, `Interface` | `Unbuffered` | apply ASAP, no buffer-splitting cost |

This means **two** segment arrays (`coefs_auto[]`, `coefs_fast[]`), each packing
only its own policy class, so an editor-only parameter never triggers a 32-sample
buffer split. It also replaces today's `_to_processor` lock-free queue
([parameters.hpp:190-192](../wrappers/aax/source/parameters.hpp#L190-L192)), which
exists *only* because non-automatable params are not synchronized — the last
direct-memory channel from the GUI to the audio thread.

---

## 4. The proposed algorithm context

```cpp
#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_ALG
#include AAX_ALIGN_FILE_END

struct Alg_context {
    // --- host-provided environment ---
    float*        * audio_in;          // AddAudioIn
    float*        * audio_out;         // AddAudioOut
    int32_t       * num_frames;        // AddAudioBufferLength
    float         * sample_rate;       // AddSampleRate
    int32_t       * sidechain_index;   // AddSideChainIn (or filler)
    AAX_IMIDINode * transport_node;    // AddMIDINode(Transport)
    AAX_IMIDINode * midi_in;           // AddMIDINode(LocalInput) or filler  [midi-support]
    AAX_IMIDINode * midi_out;          // AddMIDINode(LocalOutput) or filler [midi-support]

    // --- inbound packets ---
    const Config_packet  * config;     // sample rate, max block, static setup
    const Runtime_packet * runtime;    // offline/recording/delay-comp, accepted latency
    const Coef_segment   * coefs_auto[num_auto_segments];
    const Coef_segment   * coefs_fast[num_fast_segments];

    // --- algorithm-owned persistent state ---
    Alg_state * state;                 // AddPrivateData(sizeof(Alg_state))

    // --- outbound to host ---
    float* * meter_taps;               // AddMeters (optional; see §8)
};
#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_RESET
#include AAX_ALIGN_FILE_END
```

`Alg_state` is the algorithm's whole world:

```cpp
struct Alg_state {
    plugin::Processor processor;                 // the user's DSP kernel
    Host_bypass       bypass;                    // moved off the data model
    double            shadow[num_params];        // for segment diffing
    uint64_t          shadow_seq_auto[num_auto_segments];
    uint64_t          shadow_seq_fast[num_fast_segments];
    std::array<double, num_meters> meter_accum;  // Dsp_context::meters target

    Return_ring       returns;                   // §8: meters/blocks/worker/latency out
    uint32_t          accepted_latency;
    bool              constructed;
};
```

`sizeof(Alg_state)` is a compile-time constant → `AddPrivateData` size. Note that
this bounds the *object*, not its heap allocations: `plugin::Processor` may
allocate normally in `reset(sr)` on Native (§5). Private data blocks are documented
as **never relocated** between render callbacks, so pointers into `Alg_state` are
stable.

---

## 5. Where the processor lives: private data + InstanceInit

The obvious worry: `plugin::Processor::reset(sample_rate)` allocates (delay lines,
FFT plans), and the algorithm callback is real-time. Where does construction
happen?

**Answer: the `AAX_CInstanceInitProc`,** registered as the third argument to
`AddProcessProc_Native`. Per the SDK it is called *in the algorithm's memory space*,
"before the instance appears in the list supplied to CProcessProc", with a 5-second
budget, and — critically — **step 5 of the initialization order, i.e. after all
packets have been delivered (step 4)**. So the `config` packet, posted by the data
model from `EffectInit` where `Controller()->GetSampleRate()` is available, is
already in the context:

```cpp
auto AAX_CALLBACK alg_init(const Alg_context* ctx, AAX_EComponentInstanceInitAction action) -> int32_t
{
    auto* st = ctx->state;
    switch (action) {
        case AAX_eComponentInstanceInitAction_AddingNewInstance:
            new (st) Alg_state{};                       // placement new, off the RT thread
            st->processor.reset(ctx->config->sample_rate);
            st->bypass.reset(ctx->config->sample_rate);
            st->constructed = true;
            return 0;
        case AAX_eComponentInstanceInitAction_ResetInstance:
            st->processor.reset(ctx->config->sample_rate);
            return 0;
        case AAX_eComponentInstanceInitAction_RemovingInstance:
            st->~Alg_state();
            return 0;
    }
    return 0;
}
```

This sidesteps the relocation hazard entirely. **Do not construct in
`ResetFieldData`**: that runs on the host and the block "is then copied into the
algorithm's memory pool", which would require `Alg_state` to be trivially
relocatable — an unacceptable constraint on user DSP code. `ResetFieldData` should
just zero the block (the base-class default).

Belt-and-braces: the render callback checks `st->constructed` and returns silently
(writing silence to outputs — the SDK requires *all* output samples be written)
if the init proc did not run. See §15 for verification.

Sample-rate changes are not a practical concern: Pro Tools fixes the session sample
rate for the lifetime of the session, and `ResetInstance` covers the reset path.

---

## 6. Responsibility migration table

Everything currently in `Parameters::RenderAudio`
([parameters.cpp:567-741](../wrappers/aax/source/parameters.cpp#L567-L741)),
mapped to its decoupled home.

| Today (monolithic) | Decoupled home | Mechanism |
|---|---|---|
| Synchronized param values → `handle_event(Set_param)` | algorithm | `Buffered` coef segments + shadow diff (§3) |
| `_to_processor` queue for non-automatable params | algorithm | `Unbuffered` coef segments (§3) |
| Plain-space conversion in `RenderAudio` | **data model** | in `GenerateCoefficients`, off the RT thread |
| `_bypass` (`Host_bypass`) processing | algorithm | lives in `Alg_state`; bypass param arrives as a coef |
| `_bypass.set_latency()` | algorithm | from `Runtime_packet::accepted_latency` |
| `Controller()->SetSignalLatency` on propose | data model | algorithm writes propose → Direct Data reads → `SetSignalLatency` (§8) |
| `_pending_latency` / `_accepted_latency` atomics | split | propose out via Direct Data; accepted in via `Runtime_packet` |
| Meter write to `_meter_queue` | algorithm → data model | Direct Data ring (§8) |
| Transport reads (`GetTransport()`, tempo, position) | algorithm | `AddMIDINode(Transport)`; `AAX_IMIDINode::GetTransport()` |
| `_offline` / `_recording` / `_delay_comp` from `NotificationReceived` | data model → algorithm | `Runtime_packet` on an `Unbuffered` port |
| Worker `_worker_from_proc.push` on the RT thread | algorithm → data model | Direct Data ring (§8) |
| Worker `_worker_to_proc` drain in `RenderAudio` | data model → algorithm | packet on an `Unbuffered` port, or Direct Data write |
| `_worker_from_edit` / `_worker_to_edit` | **unchanged** | editor and worker both live on the data model |
| `GetChunk` / `SetChunk` / `CompareActiveChunk` | **unchanged** | pure data model; already touches no algorithm state |
| Undo history, `State_adapter`, editor `notify` | **unchanged** | data model |
| Editor window-size cache | **unchanged** | data model |
| GUI (`gui.cpp`) parameter get/set/touch/release | **unchanged** | already goes through `AAX_IParameter` only |
| GUI `pop_meter` | **unchanged interface** | queue is now fed by Direct Data instead of `RenderAudio` |
| GUI `push_action` for non-automatable params | **deleted** | subsumed by `Unbuffered` coefs |

Two things fall out of this table that are worth stating plainly:

- **The GUI needs almost no change.** It already speaks only to the data model
  ([gui.cpp](../wrappers/aax/source/gui.cpp)). Only `push_action` disappears.
- **State/chunk handling needs no change at all.** ~330 lines of
  `parameters.cpp` (chunk build/parse/compare, undo capture, host-load notify) are
  untouched.

---

## 7. Parameter flow in detail

### Data model side

```cpp
class Parameters : public AAX_CEffectParameters {
    AAX_Result UpdateParameterNormalizedValue(AAX_CParamID id, double v, AAX_EUpdateSource src) override
    {
        const auto r = AAX_CEffectParameters::UpdateParameterNormalizedValue(id, v, src);
        if (const auto tiny_id = aax_id_to_tiny(id)) {
            _mark_segment_dirty(*tiny_id);
        } else if (std::strcmp(id, cDefaultMasterBypassID) == 0) {
            _mark_segment_dirty(bypass_pseudo_address);
        }
        return r;
    }

    AAX_Result GenerateCoefficients() override
    {
        for (auto seg : _dirty_auto.take()) _post_segment(_auto_map, seg, coef_auto_field(seg));
        for (auto seg : _dirty_fast.take()) _post_segment(_fast_map, seg, coef_fast_field(seg));
        return AAX_SUCCESS;
    }
};
```

`_post_segment` fills a `Coef_segment` from `mParameterManager` (using the existing
`get_aax_param` / `Value_helper` conversions), bumps `seq`, and calls
`Controller()->PostPacket(field, &seg, sizeof(seg))`.

We do **not** use `AAX_CPacketDispatcher`. It is a convenience layer built on
`std::map<std::string, ...>` plus a mutex, keyed by parameter ID string — exactly
the wrong data structure when the mapping is a compile-time integer division.
Our own dirty-segment bitset is smaller, faster, and allocation-free.

### Algorithm side

```cpp
template<int32_t num_channels>
void AAX_CALLBACK alg_proc(Alg_context* const begin[], const void* end)
{
    for (auto** walk = begin; walk < end; ++walk) {
        auto* ctx = *walk;
        auto* st  = ctx->state;
        if (!st->constructed) { write_silence(ctx); continue; }

        // 1. Accepted latency (from Runtime_packet).
        if (const auto lat = ctx->runtime->accepted_latency; lat != st->accepted_latency) {
            st->accepted_latency = lat;
            st->processor.handle_event(Accepted_latency{lat});
            st->bypass.set_latency(lat);
        }

        // 2. Coefficient segments -> Set_param events.
        apply_segments(ctx->coefs_auto, st->shadow_seq_auto, st->shadow, st->processor);
        apply_segments(ctx->coefs_fast, st->shadow_seq_fast, st->shadow, st->processor);

        // 3. Transport -> Musical_context (unchanged logic, moved).
        // 4. Dsp_context, process, bypass, meters -> st->returns.
        // 5. propose_latency -> st->returns.
    }
}
```

`Set_param` events are emitted at the top of the callback, i.e. offset 0 of a
buffer the host has already split for us — **exactly today's semantics, but with
32-sample accuracy instead of full-buffer accuracy.**

---

## 8. The return channel: Direct Data

Everything flowing *out* of the algorithm — meters, blocks, worker messages,
latency proposals — uses the `AAX_IEffectDirectData` module, registered in Describe
with `AddProcPtr(..., kAAX_ProcPtrID_Create_EffectDirectData)`.

Its API is exactly what we want:

```cpp
AAX_Result ReadPortDirect (AAX_CFieldIndex field, uint32_t offset, uint32_t size, void* out);
AAX_Result WritePortDirect(AAX_CFieldIndex field, uint32_t offset, uint32_t size, const void* in);
```

**A memcpy of a byte range — value semantics by construction**, and the mechanism
works identically on Native, AudioSuite and (in principle) HDX. It is the direct
analogue of VST3's `IMessage`.

### Characteristics and constraints

- Delivered via `TimerWakeup(AAX_IPrivateDataAccess*)`, **"approximately one call
  per 30 ms"**, on a non-main thread, with no regularity guarantee ("could be held
  off by a high real-time load").
- Reads/writes are **blocking** and **not synchronized with the algorithm
  callback** — the SDK says explicitly: "Plug-ins that use this interface should
  buffer all access to their private data to ensure data integrity."
- The `AAX_IPrivateDataAccess` pointer is valid only inside the callback.

### The `Return_ring`

A single SPSC ring inside `Alg_state`, written by the algorithm (RT thread), read
by the Direct Data timer. Because reads are per-byte-range and not atomic with
respect to the writer, the drain protocol is a three-step read with a re-check:

1. `ReadPortDirect` the 8-byte header `{write_index, generation}`.
2. `ReadPortDirect` the byte range `[read_index, write_index)` (wrapping = two reads).
3. `ReadPortDirect` the header again; if `write_index` advanced far enough to have
   overwritten what we just read, discard and retry next wakeup.
4. `WritePortDirect` the new `read_index` back.

Sizing the ring for ≥ 2× the worst-case 30 ms production makes step 3 fire
essentially never. Entries are a small POD variant:

```cpp
struct Return_entry {
    enum class Kind : uint32_t { meter, block_chunk, worker_msg, propose_latency };
    Kind     kind;
    uint32_t address;
    uint32_t payload_bytes;
    // followed by payload_bytes of data, 8-byte aligned
};
```

The Direct Data object then hands each entry to the data model via
`AAX_IEffectParameters::SetCustomData()` — the SDK's documented inter-module
channel ("to communicate with the plug-in's data model, use `GetCustomData()` /
`SetCustomData()`") — which pushes into the *existing* `_meter_queue`,
`_worker_from_proc`, `Block_channel`, or `_pending_latency`. **The GUI's
`Ui_receiver` wiring is unchanged.**

### Meters: why not use host meters?

AAX has a built-in metering path (`AddMeters` in the algorithm,
`AAX_IController::GetCurrentMeterValue()` in the GUI, host-applied ballistics and
thinning). It is tempting — it is polled at GUI frame rate rather than 33 Hz — but
it does not preserve `meters::Policy`
([tiny_meters.hpp](../libs/tinyplug/include/tinyplug/tiny_meters.hpp)):

| Policy | Host-meter behaviour | Verdict |
|---|---|---|
| `Peak` | host thins with **max** — exactly our semantics | fits |
| `Stream` ("latest value") | max-thinning holds a *decreasing* value high across a thin window | **wrong** |
| `Trig` ("one frame") | a poll cannot observe a one-shot; ballistics smear it | **wrong** |

Also, host meters are 0…1 (our `meters::Range` would need normalize/denormalize)
and every registered meter shows up in Pro Tools' plug-in header and on control
surfaces — undesirable for a plug-in with a dozen internal analysis meters.

**Recommendation: framework meters go through the `Return_ring`.** Peak accumulation
happens in `Alg_state::meter_accum` on the RT thread exactly as today, so the 33 Hz
drain loses nothing for `Peak`, and `Stream`/`Trig` keep their exact semantics
because they are *queued entries*, not polled values.

Optionally, and additively: let `meters::Spec` gain an AAX hint
(`AAX_eMeterType_Input`/`Output`/`CLGain`) so an author can *also* publish a meter
to Pro Tools' header and control surfaces via `AddMeters`. That is a feature, not a
migration requirement — defer it.

### Latency

Straight out of the SDK's own example
([DemoGain_Background/DemoGain_DirectData.cpp](../../tiny_deps/third_party/aax-sdk/ExamplePlugIns/DemoGain_Background/Source/DemoGain_DirectData.cpp)):

1. Algorithm sets `propose_latency` → pushes a `propose_latency` entry.
2. Direct Data timer reads it, calls `Controller()->SetSignalLatency(n)`.
3. Host dispatches `AAX_eNotificationEvent_SignalLatencyChanged` to the data model.
4. Data model reads back `GetSignalLatency()` (the host owns latency in AAX) and
   posts it in the next `Runtime_packet`.
5. Algorithm sees the changed `accepted_latency`, issues `Accepted_latency{n}` to
   the kernel and updates `Host_bypass`.

Identical in shape to the existing protocol in [CLAUDE.md](../CLAUDE.md); the only
change is that steps 1 and 4 cross a real boundary instead of an atomic.

---

## 9. Feasibility: [block-output.md](block-output.md)

**Verdict: better in the two-component design than in the monolithic one.** This is
the single strongest technical argument for the migration.

The Direct Data documentation describes this exact use case verbatim:

> Some plug-ins require the host to retrieve non-meter data from the decoupled
> algorithm module to display on a GUI... For example, the result of computing the
> audio spectrum or pitch data in the algorithm can be delivered to the host to
> display on-screen. This is the purpose of the `AAX_IEffectDirectData` interface.

Mapping to the plan:

| block-output concept | AAX two-component |
|---|---|
| `Block_channel` (triple buffer / SPSC ring) | lives **twice**: producer side in `Alg_state`, consumer side on the data model |
| `Dsp_context::blocks.write_buffer()/publish()` | writes into an `Alg_state`-resident staging buffer; `publish()` enqueues a `block_chunk` entry (or bumps a per-block `seq`) |
| transport across the boundary | `ReadPortDirect` memcpy on the Direct Data timer |
| `Ui_receiver::read_block` | reads the **data-model-side** `Block_channel` — unchanged from §5.1 of the plan |

Sizing: a 1024-bin `snapshot` spectrum is 4 KB; at the 33 Hz timer that is ~135 KB/s
of memcpy — negligible on Native, and within HDX's ~10 MB/s guidance if it ever
matters. For `snapshot` policy the ring is unnecessary — a plain double-buffer with
a `seq` counter in private data suffices: the timer reads `seq`, reads the buffer,
re-reads `seq`, retries on mismatch (a seqlock). `stream` policy uses the
`Return_ring`.

The 33 Hz cap is the real cost: a `stream` oscilloscope will arrive in bursts of
1–2 blocks every 30 ms rather than smoothly. For visualization this is acceptable
and matches the plan's own tolerance for dropped blocks.

**Note the correction needed to block-output.md §5.1**, which currently lists AAX
alongside CLAP/AUv2/AUv3 as "the framework's own channels, used directly". Under
this design AAX moves to the VST3 column: a transported channel, not a shared one.

---

## 10. Feasibility: [buffer-system.md](buffer-system.md)

**Verdict: the *persistence* leg is fine once principle 2 is stated more generally.
The *install* leg is the one genuine conflict, and it needs an explicit decision.**

### First: restate principle 2

buffer-system.md principle 2 reads *"the processor owns the canonical source
bytes"*, with the corollary *"the canonical bytes live where `getState` lives, so
nothing heavy ever crosses the COM boundary."*

That phrasing hides a coincidence. In VST3 the component that owns `getState` for
parameters and the component that runs `process()` are **the same object**
(`Vst3_processor`). So "the processor owns the source" and "the install pointer
swap is component-local" are, there, the same statement. The same is true of every
other format we ship — including AAX today.

The general rule is:

> **The canonical source bytes live in whichever component the format makes
> responsible for state.** Persistence is then always component-local, for free, in
> every format.

Under that phrasing nothing about VST3, CLAP, AUv2, AUv3 or monolithic AAX changes.
What changes is that the second half of the corollary — *and that component is also
the one running the audio thread* — is revealed as a **separate** property that
happens to hold everywhere except one place:

| Format | Owns state | Runs RT audio | Same component? |
|---|---|---|---|
| VST3 | `Vst3_processor` | `Vst3_processor` | yes |
| CLAP | `Clap_plugin` | `Clap_plugin` | yes |
| AUv2 / AUv3 | `Effect` / `Auv3_AUAudioUnit` | same | yes |
| AAX **monolithic** | `Parameters` | `Parameters::RenderAudio` | yes |
| **AAX two-component** | **data model** | **algorithm** | **no** |

So yes — to answer the framing question directly: **"the canonical bytes live where
the format wants them to live" is exactly right, and it is Option 1 below on its
own.** It resolves persistence completely. Option 2 is a *separate* concession that
addresses a *different* leg, and only AAX two-component ever needs it.

### The four legs, re-examined

| Leg | Crosses in AAX two-component? | Notes |
|---|---|---|
| **Persist** (`source` ↔ chunk) | **no** | source sits on the data model, next to `GetChunk`. The `'tbuf'` second-chunk scheme in buffer-system.md is unaffected. |
| **Acquire** (editor → source) | **no** | editor and data model are the same component. Even simpler than VST3, which needs `IMessage` here. |
| **Capture** (looper writes live audio) | **yes** | the capture scratch must live in `Alg_state`, so `Buffer_spec::max_frames × 2` becomes part of the private-data block size; the committed bytes then travel algorithm → data model for `encode_buffer` + persistence. Bounded and off-thread — Direct Data handles it. |
| **Install** (`shared_ptr<const Prepared>` → audio thread) | **yes** | **the conflict.** A pointer cannot cross a component boundary in principle, and `Prepared` is unbounded by design. |

### Options for the install leg

1. **Slots on the data model.** Note that buffer-system.md already binds the
   processor to buffers through `Dsp_context::buffers`, a struct of
   `std::function`s — i.e. **the framework owns slot storage, not the user's
   `Plug_processor`.** So `source`, the prepare job, and `Prepared` allocation all
   live on the data model. This is the restatement above; it fixes persist,
   acquire, and prepare, and leaves only the install pointer.

2. **Compile-time-gated co-location.**
   `AAX_eProperty_Constraint_Location = AAX_eConstraintLocationMask_DataModel` —
   the same property the vendored monolith sets unconditionally today — guarantees
   the algorithm runs in the data model's address space:

   ```cpp
   if constexpr (Buffer_infos<models::Buffers>::num_buffers > 0) {
       properties->AddProperty(AAX_eProperty_Constraint_Location,
                               AAX_eConstraintLocationMask_DataModel);
   }
   ```

   `Set_buffer{addr, key, shared_ptr<const Prepared>}` then travels as a pointer
   payload in an `Unbuffered` packet. Cost: that plug-in forfeits HDX/TI — which a
   sampler forfeits anyway.

3. **Bounded private-data mirror.** `AddPrivateData(max_buffer_bytes)` +
   `WritePortDirect` the prepared PCM across. Genuinely distributable, but needs a
   compile-time maximum size and pushes megabytes through a 30 ms blocking timer.
   Fine for a wavetable or a short IR; hopeless for a sampler.

4. **AAX Hybrid.** Worth recording because Avid names our exact use case. Hybrid
   registers a *second* render callback that "is implemented directly within the
   plug-in's effect parameters object and **has direct access to the data model
   memory** ... makes it easier to implement algorithms that require access to the
   data model, **e.g. for direct access to impulse responses**". Audio is routed
   between the low-latency ProcessProc and the Hybrid callback through extra
   host-provided buffers.
   **Rejected**, for three reasons: it imposes a fixed round-trip latency
   (`GetHybridSignalLatency`); parameter updates reach the Hybrid context ~21 ms
   *early*, breaking our timing model; and it is not supported by all AAX hosts. It
   is designed for splitting a reverb tail off to the host, not for putting a whole
   sampler there — a looper whose entire DSP consumes the buffer would end up as
   "monolithic, plus latency".

5. **Declare buffers unsupported on AAX.** Not acceptable — Pro Tools is a
   first-class target for exactly the sampler/looper class.

### Recommendation

**Option 1 unconditionally** (it is just the corrected principle, and it costs
nothing anywhere), **plus Option 2 gated on `num_buffers > 0`.**

Document the pointer in `Set_buffer` as *the* named exception to AAX's
value-semantics rule — confined to one payload type, on one port, in plug-ins that
have explicitly opted into buffers — the same way VST3's `kDistributable` has its
documented `Outbound_message_shuttle` exception. Zero-buffer plug-ins (all of them
today) declare no constraint and stay strictly decoupled.

**Follow-up for buffer-system.md:** reword principle 2 from "the processor owns the
canonical source" to "the state-owning component owns the canonical source", and
add the ownership table above. This is a clarification, not a change — no format's
behaviour moves.

Worth a note back in buffer-system.md once this plan is accepted.

---

## 11. Feasibility: [midi-support.md](midi-support.md)

**Verdict: no obstacle; a small correction to the plan's §6.5.**

`AAX_IComponentDescriptor::AddMIDINode(fieldIndex, type, name, channelMask)` is a
**component-level** call — MIDI nodes are algorithm context fields, not a monolithic
convenience. All four node types (`LocalInput`, `LocalOutput`, `Global`, `Transport`)
are available to a decoupled algorithm. The SDK's instrument examples all happen to
use `AAX_CMonolithicParameters`, which is a convenience choice, not a constraint —
and `AAX_CMonolithicParameters::StaticDescribe` itself just forwards to
`compDesc->AddMIDINode`.

midi-support.md §6.5 says "`RenderAudio()`: iterate `ioRenderInfo->mInputNode->GetNodeBuffer()`".
Under this design that becomes "`alg_proc()`: iterate `ctx->midi_in->GetNodeBuffer()`" —
same code, different owner. Timestamped `AAX_CMidiPacket`s land in the same render
buffer, so sample-offset MIDI events work identically.

One improvement: instrument plug-ins get the *better* automation timing described
in §2, which matters more for synths than for effects.

---

## 12. What we lose, and residual risks

| # | Issue | Severity | Mitigation |
|---|---|---|---|
| 1 | **Processor→editor latency rises from "next GUI frame" to ~30 ms** (Direct Data timer). | Medium | Peak meters accumulate on the RT thread, so no data is lost — only update rate. 33 Hz metering is standard. `Trig` events are queued, not polled, so none are dropped. |
| 2 | Direct Data timer has **no regularity guarantee** and can be held off under RT load. | Medium | Size `Return_ring` for ≥ 2× worst-case; drop-oldest on overflow for meters/blocks (already the policy), never for worker messages (assert instead, as today). |
| 3 | `Alg_state` size is fixed at Describe time. | Low | It bounds the *object*, not its heap allocations. Only the buffer system's capture scratch needs an explicit compile-time bound — which its `Buffer_spec::max_frames` already declares. |
| 4 | **Buffer install needs a co-location escape hatch.** | Medium | §10; gated by `if constexpr`, documented, one payload type. |
| 5 | Host may split buffers to 32 samples during automation — more callbacks, more per-callback overhead. | Low | This is normal AAX behaviour that every conventional AAX plug-in already lives with, and it is *why* AAX automation is accurate. Our per-callback fixed cost is small (segment `seq` checks). |
| 6 | `InstanceInit` reliability / whether the `config` packet is populated at step 5. | **Needs verification** | §15 Q1. Fallback: `AddSampleRate` field + lazy first-callback init behind `constructed`, accepting one allocating callback at instantiation (before audio is critical). |
| 7 | More moving parts: 4 modules (Describe, data model, algorithm, Direct Data) instead of 3. | Low | Offset by deleting ~760 lines of vendored monolith. |
| 8 | Migration touches the most-tested wrapper. | Medium | Phased plan (§14) with the monolithic path kept behind a CMake switch until parity is proven in Pro Tools + AAX validator. |
| 9 | `CompareActiveChunk` / Pro Tools compare light. | None | Data-model-only; untouched. |

---

## 13. Three designs, and the recommendation

**A — Status quo (monolithic).** Zero risk, zero gain. Keeps the vendored monolith,
keeps buffer-granular automation, keeps AAX as the one wrapper where the
framework's processor/editor split is unenforced, and gives block-output no natural
transport.

**B — Two-component in form, co-located in fact.** Split the components and use
packets for parameters, but keep `AAX_eConstraintLocationMask_DataModel`
unconditionally and let the algorithm hold a pointer to data-model-side state for
meters/worker/blocks. Gets win #2 (automation timing) and win #1 (delete the
monolith) cheaply, skips the Direct Data work entirely. But it does *not* get win #3
— the decoupling would be cosmetic, and the first person to add a feature would
reach through the pointer.

**C — Strict decoupling.** As designed above: nothing crosses except packets in and
Direct Data out.

**Recommendation: C, staged, with B as the phase-1 milestone rather than a
destination.**

Concretely: land the parameter/coefficient half first (phases 1–3 below), which is
where the automation-timing win and the monolith deletion live and where the risk
is lowest. Then build the Direct Data return channel (phase 4) and cut the last
shared-memory paths. Do **not** ship B and stop — the whole point is win #3, and a
co-location constraint left in place unconditionally will quietly get depended on.

Set `AAX_eProperty_Constraint_Location` to **none** from phase 4 onward, adding it
back only under `if constexpr (num_buffers > 0)`.

Rough size: ~800 lines of new/rewritten wrapper code, ~760 lines of vendored code
deleted, ~330 lines of chunk/state code untouched, `gui.cpp` nearly untouched.

---

## 14. Implementation sequence

Each phase is independently testable in Pro Tools and against the AAX validator.

1. **Scaffolding.** New `wrappers/aax/source/alg_context.hpp` (context struct,
   segment layout, field-index helpers, `params_per_segment` math) and
   `alg_proc.cpp` (empty pass-through algorithm). Rewrite `describe.cpp` to build a
   real `AAX_IComponentDescriptor` instead of calling `StaticDescribe`. Keep the
   monolithic path alive behind a CMake option so both can be built and A/B'd.

2. **Parameters + audio.** Coefficient segments end-to-end: dirty tracking in
   `UpdateParameterNormalizedValue`, `GenerateCoefficients` posting, shadow diff in
   the algorithm, `Set_param` → `Processor::handle_event`. Move `Host_bypass` and
   the master-bypass parameter into `Alg_state`. `Config_packet` + `InstanceInit`
   construction (§5). Transport node. Sidechain. At the end of this phase the
   plug-in makes correct sound with correct automation.

3. **Delete the monolith.** Remove `monolith.hpp`/`monolith.cpp` and the
   `AddSynchronizedParameter` calls from `EffectInit`; drop the CMake switch. Update
   [CLAUDE.md](../CLAUDE.md) (the "vendored monolith" section, the AAX bullet in
   "Per-format quirks", and the latency-protocol description).

4. **Direct Data return channel.** `Return_ring` in `Alg_state`; `Aax_direct_data :
   AAX_CEffectDirectData` registered via `kAAX_ProcPtrID_Create_EffectDirectData`;
   `SetCustomData` bridge into the existing `_meter_queue` / `_worker_from_proc` /
   `_pending_latency`. Meters and worker replies stop using shared memory. Remove
   `AAX_eProperty_Constraint_Location`.

5. **Latency handshake** over the new channel; verify against the five-step protocol
   in CLAUDE.md and Pro Tools' delay compensation display.

6. **Validator + host matrix.** AAX validator (`../aax-validator`), Pro Tools
   Native, AudioSuite, offline bounce, multi-mono, mono + stereo variants,
   Media Composer if convenient.

7. **Plan follow-ups.** Amend [block-output.md](block-output.md) §5.1/§5.3 to move
   AAX into the transported column; amend [buffer-system.md](buffer-system.md) with
   the §10 principle-2 rewording and co-location note; amend
   [midi-support.md](midi-support.md) §6.5 to
   reference the algorithm context rather than `RenderAudio`.

---

## 15. Open questions to verify empirically

These are the things the documentation does not settle. Each has a stated fallback,
so none is a blocker — but Q1 and Q3 should be answered with a spike before phase 2.

**Q1. Is the `config` data-in port populated when `AAX_CInstanceInitProc` runs?**
The initialization order says packets are delivered (step 4) before the init
callback (step 5) and that the callback receives "the algorithm's default context",
but does not say the callback's context has live port pointers.
*Fallback:* register `AddSampleRate` and read `*ctx->sample_rate` in the init proc;
failing that, lazy-init on the first render callback behind `constructed`.

**Q2. Does Pro Tools call `AAX_CInstanceInitProc` with `RemovingInstance` reliably
on teardown?** If not, `Alg_state`'s destructor never runs and the user
`Processor`'s heap allocations leak per instance.
*Fallback:* have the data model own a registry of live `Alg_state*` (learned via
the first render or via `ResetFieldData`) and destroy on `~Parameters`. Ugly;
verify first.

**Q3. What is the real-world Direct Data wakeup rate under load in Pro Tools?**
The SDK says ~30 ms with no guarantee. Meter smoothness and block frame rate depend
on it.
*Measurement:* timestamp wakeups in a debug build across playback, heavy sessions,
and offline bounce.

**Q4. Does `ReadPortDirect` scale to a few KB per wakeup without stalling?**
It is documented as blocking. A 4 KB spectrum read plus a 2 KB return-ring drain,
33× per second, per instance, across 50 instances is ~10 MB/s aggregate.
*Fallback:* rate-limit blocks per instance; coalesce reads into one call by placing
all return data in a single contiguous private-data block (which the design already
does).

**Q5. Do multiple `AddPrivateData` blocks or one large block behave differently?**
The design uses one `Alg_state` block. Confirm no per-block size cap bites for a
plug-in with a large capture scratch.

**Q6. Does the AAX validator flag anything about high buffered-port counts on
Native?** The 164-port cap is documented for HDX only.

**Q7. `AAX_eProperty_PlugInID_RTAS` vs `_Native` — RESOLVED, no risk.**
`AAX_Properties.h:141` defines `AAX_eProperty_PlugInID_RTAS = AAX_eProperty_PlugInID_Native`
— the same enum value, with `_RTAS` marked deprecated. The new Describe uses
`_Native`; session recall is unaffected.

**Q8. Do the six demos behave identically in Pro Tools?** The port of the wrapper is
complete and compiles clean, but nothing here has been exercised in a host yet. The
first pass should check, per demo: audio identity vs. the monolithic build, automation
write/read, meter smoothness (`AutomationTester`), latency reporting and PDC
(`LatencyDemo`), worker round-trips (`WorkerDemo`), offline bounce (`RenderModeDemo`),
and preset save/recall + the compare light on all of them.

---

## 16. Key files

| File | Change |
|---|---|
| `wrappers/aax/source/alg_context.hpp` | **New** — context struct, segment layout, field-index helpers |
| `wrappers/aax/source/alg_proc.cpp` | **New** — the algorithm callback + `InstanceInit` |
| `wrappers/aax/source/direct_data.hpp/.cpp` | **New** — `AAX_CEffectDirectData` subclass, return-ring drain |
| `wrappers/aax/source/return_ring.hpp` | **New** — SPSC ring designed for `ReadPortDirect` access |
| `wrappers/aax/source/describe.cpp` | Rewritten — real component descriptor, ports, procs, properties |
| `wrappers/aax/source/parameters.hpp/.cpp` | Dirty-segment tracking, `GenerateCoefficients`, `SetCustomData` bridge; `Host_bypass`, meter queue producer and `_to_processor` removed |
| `wrappers/aax/source/monolith.hpp/.cpp` | **Deleted** (phase 3) |
| `wrappers/aax/source/gui.cpp` | `push_action` path removed; otherwise unchanged |
| `wrappers/aax/make_aax_plugin.cmake` | Source list; temporary A/B option in phases 1–2 |
| `plans/block-output.md` | AAX moves to the transported column (§9) |
| `plans/buffer-system.md` | Principle 2 reworded to "state-owning component"; co-location note (§10) |
| `plans/midi-support.md` | §6.5 retargeted at the algorithm context (§11) |
| `CLAUDE.md` | AAX section rewritten: no vendored monolith, new latency path, two-component quirks |

---

## 17. Source references

Everything above was checked against the vendored SDK at
`../tiny_deps/third_party/aax-sdk`.

**Documentation**
- `Documentation/Doxygen/dox/AAX_CommonInterface_Algorithm.doxygen` — context
  structure, memory management, port types, private data, initialization order,
  optional init callback.
- `Documentation/Doxygen/dox/AAX_CommonInterface_Describe.doxygen` — descriptor
  hierarchy, component description, property maps.
- `Documentation/Doxygen/dox/AAX_CommonInterface_Communication.doxygen` — packets,
  custom data (`Get/SetCustomData`), notifications, direct pointer sharing and the
  `Constraint_Location` escape hatch.
- `Documentation/Doxygen/dox/AAX_ParameterUpdateTiming.doxygen` — timestamped
  packets, the 32-sample Native buffer-splitting guarantee, why monolithic plug-ins
  need the deferred-update machinery we are deleting.
- `Documentation/Doxygen/dox/AAX_ParameterUpdateProtocol.doxygen` +
  `Documentation/Doxygen/msc/AAX_ParameterUpdate_GUI.msc` — `GenerateCoefficients()`
  is called by the host following `UpdateParameterNormalizedValue()`.
- `Documentation/Doxygen/dox/AAX_AuxInterface_DirectData.doxygen` — the return
  channel; the ~30 ms wakeup; "buffer all access".
- `Documentation/Doxygen/dox/AAX_AdditionalFeatures_Meters.doxygen` — host metering
  semantics and its explicit pointer to Direct Data "for advanced metering
  applications".
- `Documentation/Doxygen/dox/AAX_AdditionalFeatures_Algorithm.doxygen` — background
  proc; private data is never relocated.
- `Documentation/Doxygen/dox/AAX_AdditionalFeatures_Hybrid.doxygen` — the Hybrid
  render callback with direct data-model memory access ("e.g. for direct access to
  impulse responses"), its round-trip latency, its ~21 ms early parameter updates,
  and its partial host support. Evaluated and rejected in §10.
- `Documentation/Doxygen/dox/AAX_TI_Guide.doxygen` — 128-byte minimum transfer,
  buffered-port CPU cost, 164-port cap, consolidated-packet recommendation.
- `Documentation/Doxygen/dox/AAX_BugList.doxygen` — PTSW-187216 (`PostPacket` in
  `TimerWakeup` breaks unbuffered ports), PTSW-158119 (port cap), PT-206161
  (`PostPacket` outside `GenerateCoefficients`).

**Headers**
- `Interfaces/AAX.h:296` — `AAX_FIELD_INDEX` = `offsetof / sizeof(void*)`.
- `Interfaces/AAX_IComponentDescriptor.h` — all `Add*` field registrations;
  `AddProcessProc_Native(proc, props, initProc, backgroundProc)`.
- `Interfaces/AAX_IController.h` — `PostPacket`, `Get/SetSignalLatency`,
  `GetCurrentMeterValue`.
- `Interfaces/AAX_IPrivateDataAccess.h` — `ReadPortDirect` / `WritePortDirect`.
- `Interfaces/AAX_CEffectDirectData.h`, `Interfaces/AAX_IACFEffectDirectData.h` —
  `TimerWakeup`, "approximately one call per 30 ms".
- `Interfaces/AAX_Enums.h` — `AAX_EDataInPortType`, `AAX_EPrivateDataOptions`,
  `AAX_EConstraintLocationMask`, `AAX_EComponentInstanceInitAction`,
  `AAX_EMeterType`, `AAX_EMeterBallisticType`, `AAX_EMIDINodeType`,
  `AAE_EAudioBufferLengthNative`.
- `Interfaces/AAX_Callbacks.h` — `AAX_CInstanceInitProc` semantics.
- `Libs/AAXLibrary/source/AAX_CPacketDispatcher.cpp` — `Dispatch()` is per-port,
  dirty-flag driven (why we can replace it with a bitset).

**Examples**
- `ExamplePlugIns/DemoDelay/` — the canonical decoupled effect: coefficient struct,
  data-in ports, private data delay line, meters, `ResetFieldData` placement new.
- `ExamplePlugIns/DemoDist_GenCoef/` — multiple coefficient ports and consolidated
  coefficient generation.
- `ExamplePlugIns/DemoGain_Background/DemoGain_DirectData.cpp` — the
  latency-proposal-out / latency-accepted-in pattern over `Read/WritePortDirect`.

**Forum**
- Rob Majors (Avid), Jan 2026, on segmenting the parameter list and relying on
  agreed struct offsets rather than named fields; corroborated by the HDX guide's
  128-byte transfer minimum and consolidated-packet recommendation.
