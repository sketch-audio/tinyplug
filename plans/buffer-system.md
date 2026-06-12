# Plan: Buffer system — managed large audio buffers (looper / granular / sampler / drum machine)

> Status: **design.** This plan unifies and supersedes three earlier docs:
> the **Table** (editor → processor) half of `block-table-io.md`, all of
> `asset-store.md`, and all of `state-model.md`. The outbound-vector (**Block**)
> half of `block-table-io.md` lives on as [block-output.md](block-output.md);
> overviews are Blocks. Once this plan and `block-output.md` are accepted, those
> three docs can be deleted. The "Design history" appendix preserves the parts of
> the old framing worth keeping for posterity.

## Context

A class of plug-ins is built around **one or more large audio buffers the DSP
consumes**: loopers (captured audio), granular engines (grain sources), samplers
(one-shots / multisamples), drum machines (one buffer per pad). tinyplug models
everything *except* this — params are scalar/automatable, meters are scalar
streams out, the editor `State_map` is small typed key/values, the worker carries
trivially-copyable messages, and Blocks ([block-output.md](block-output.md)) are
transient float vectors out. There is no first-class notion of a **large,
managed, off-thread-prepared, persisted audio buffer**.

The **buffer system** is one small, opt-in, declarative model (empty by default,
mirroring params/meters/state) that owns that notion — and *only* that. It merges
what the three superseded docs each covered partially:

- `state-model.md` — persist a named blob (the buffer's canonical source).
- `block-table-io.md` Table half — get a buffer from the editor into the processor.
- `asset-store.md` — off-thread prepare + RT-safe install + deferred retire +
  capture (looper) + collections.

It does this **without** the framework-owned hub the asset-store doc proposed.

## Canonical plug-ins (the design is tested against exactly these four)

| Plug-in | Origin | Prepare | Persist | Arity |
|---|---|---|---|---|
| **Looper** | processor (captured live) | none (raw f32) | embed | single |
| **Granular** | **either** editor file *or* processor capture, one slot | decode/resample (file path); none (capture) | embed | single |
| **One-shot sampler** | editor (dropped file) | decode/resample | embed or reference | single |
| **Drum machine** | editor (file per pad) | decode/resample | embed or reference | collection |

Sequences/patterns are **out of scope** — they encode as params. Non-audio UI
state stays in the editor `State_map`. The buffer system is audio-buffer-only.

---

## Three principles that constrain the whole design

1. **Every editor ↔ processor ↔ worker leg is value semantics** (bytes or
   trivially-copyable). The VST3 controller and processor are distinct COM
   components that may live in different processes — a pointer cannot cross.
   Pointer-swap is legal **only intra-processor** (an off-thread-prepared buffer
   handed to the audio thread, same address space).

2. **The processor owns the canonical source bytes.** Editor-origin acquisition
   is just "the editor hands the processor a `Buffer_source`"; capture is "the
   processor makes a source itself." Once the processor owns the source,
   **persistence, overview generation, and re-install are all origin-agnostic and
   all processor-local.** This is the load-bearing rule: it dissolves the
   "editor loads it / processor renders it / who serializes it?" tension and makes
   **VST3 easy** — the canonical bytes live where `getState` lives, so nothing
   heavy ever crosses the COM boundary (only a small source on acquire and a small
   overview/status out).

3. **Empty by default, zero overhead.** `num_buffers == 0` ⇒ every
   `if constexpr (num_buffers > 0)` branch compiles away; no format/behaviour
   change for existing plug-ins. Same strategy as params/meters/state.

The asset-store doc's "framework-owned hub, mirrored across COM" is explicitly
**rejected** by principle 2 — there is no hub. Slots live on the processor; the
editor holds a thin push + a status/overview read.

---

## The slot (lives on the processor)

One per address (or per collection entry):

```cpp
source    : Buffer_source                          // canonical, persisted, processor-owned
prepared  : atomic<shared_ptr<const Prepared>>     // RT-installed; Prepared is author-defined, never crosses a boundary
status    : Buffer_status { empty, loading, ready, error }   // scalar, editor reads each frame
```

- **`Buffer_source`** is the canonical, *persisted* value. A variant (adopted from
  the asset-store doc) — and the **only thing that ever crosses editor → processor**,
  usually the tiny `File_ref`:

  ```cpp
  enum class Encoding { raw_f32, wav, flac /* … */ };
  struct File_ref  { std::string path; std::string sha256; };       // tiny — common desktop case
  struct Embedded  { std::vector<uint8_t> bytes; Encoding enc; };    // in-memory / captured / embed-on-save
  struct Generated { /* author-defined params */ };
  using  Buffer_source = std::variant<File_ref, Embedded, Generated>;
  ```

- **`Prepared`** is the author's RT-ready derived form (decoded PCM, windowed grain
  table, …). **Processor-local, immutable once published, never crosses any
  boundary.** It is often sample-rate / block-size dependent — which is *why* we
  persist `source` and re-derive `Prepared` on load rather than storing decoded
  data.

- **`Overview`** (waveform peaks, length, channels) for the editor is **not** part
  of the slot — it is published as a [Block](block-output.md). The buffer system
  owns content; the editor's picture of it is an ordinary Block. They compose.

---

## Declarative interface (`models/buffer_model.h`, parallel to `param_model.h`)

```cpp
enum class Buffer_origin  { editor, processor, either };   // either = granular
enum class Persist_policy { reference, embed, consolidate, transient };
enum class Buffer_arity   { single, collection };

struct Buffer_spec {
    uint32_t       address{};
    std::string    string_id{};
    std::string    name{};
    Buffer_origin  origin{Buffer_origin::editor};
    Persist_policy persist{Persist_policy::embed};
    Buffer_arity   arity{Buffer_arity::single};
    uint32_t       max_frames{};      // required for capture: looper preallocates this ×2
    uint32_t       max_entries{};     // collection bound: drum machine
};

struct Buffer_model {
    enum class Buffer_address : uint32_t { sample = 0, num_buffers };
    static auto make_specs() -> std::vector<tiny::Buffer_spec>;
};
static_assert(tiny::Some_buffer_model<Buffer_model>);
```

Default template ships `num_buffers` only + empty `make_specs()` → zero overhead.

`Some_buffer_model` / `Buffer_infos<User_model>` follow the exact shape of
`Some_state_model` / `State_infos` from the old state-model doc (concept requires
the `Buffer_address` enum + `make_specs()`; `Buffer_infos` exposes
`num_buffers`, `buffer_specs()`, `buffer_spec(addr)`).

### Processor hooks (concept-detected, opt-in — like worker replies)

```cpp
// Off-thread, processor-side. Canonical source → RT-ready form.
// Absent ⇒ Prepared is the raw buffer (looper: raw f32, no decode).
auto prepare_buffer(uint32_t addr, const Buffer_source&, const Prepare_context&) -> Prepared;

// Optional. Serialize a captured/owned Prepared back to a source for persistence
// (looper/granular embed of captured audio). Absent ⇒ the slot's existing
// `source` is persisted as-is (sampler/drum: the acquired source already is it).
auto encode_buffer(uint32_t addr, const Prepared&) -> Buffer_source;
```

`Prepare_context` carries `sample_rate`, `max_block_size`, and a decoder handle —
`max_block_size` matters because convolution partitioning / sampler buffering
depend on it.

A `static_assert` in each wrapper enforces: `num_buffers == 0 ||
Has_prepare_or_raw<Plug_processor>` (a slot with `prepare` declared needs
`prepare_buffer`; a raw slot needs none).

---

## The four legs (and where value semantics applies)

| Leg | Direction | Mechanism | Crosses the boundary? |
|---|---|---|---|
| **Acquire** | editor → proc | `edit.buffers.push(addr[, key], Buffer_source)` — usually a `File_ref` path | **value** (IMessage in VST3); tiny for `File_ref`, larger only for in-memory `Embedded` |
| **Capture** | processor-local | preallocated **double** scratch sized from `max_frames`; `ctx.buffers.capture(addr)` span written on the RT thread, `commit(addr, n)` swaps live ↔ scratch pointers (no alloc) | none |
| **Install** | proc → audio thread | `Set_buffer{addr, key, shared_ptr<const Prepared>}` on the `Render_event` queue; the **old** handle is pushed to a `retire` queue drained off-thread (deferred reclamation) | **intra-processor only** |
| **Persist** | proc ↔ session | framework pulls `source` from the slot at `getState` (off the audio thread), writes it into the chunk; load delivers it back → `prepare_buffer` → install | none (source is already processor-owned) |

### Prepare / install in detail

- **Prepare runs on the framework background executor** (`Task_manager` background
  queue), **processor-side** — *not* the user worker channels (trivially-copyable
  only; can't move big buffers). For editor-origin, prepare runs *after* the source
  has crossed to the processor, so it is local even in VST3.
- **The user `Plug_worker` is unaffected.** The prepare job is a specialized,
  framework-managed background task for large immutable buffers. The user worker
  remains for author-driven async (disk streaming, analysis, generation). A
  *disk-streaming* sampler whose content never fully resides is the user worker's
  job, not `prepare_buffer`'s — `prepare_buffer` is a one-shot derive.
- **Install via the existing event model.** When prepare finishes (or capture
  commits), the framework atomically stores the new `shared_ptr<const Prepared>`
  and enqueues a `Set_buffer` onto the processor `Render_event` queue:

  ```cpp
  struct Set_buffer {
      uint32_t address{};
      uint32_t key{};                              // collection entry; 0 for single
      std::shared_ptr<const Prepared> data{};
  };
  using Render_event = std::variant<Set_param, Ramp_param, Accepted_latency, Set_buffer>;
  ```

  The processor receives it *between* `process` calls; the `shared_ptr` copy is
  RT-safe because allocation happened off-thread. On receipt it swaps its local
  handle and pushes the old handle onto `retire`; the framework drains `retire`
  off the audio thread, so freeing megabytes never happens in `process`.
- **Capture is RT-safe by preallocation.** A looper preallocates a **double**
  max-length scratch at `reset(sr)`; `commit` swaps the live and scratch pointers
  (no alloc, no free on the audio thread). The just-committed buffer becomes both
  the playback `Prepared` (raw) and, for persistence, the `source` (via
  `encode_buffer`, possibly FLAC-compressed off-thread at save time).
- **Latency.** A new buffer (e.g. an IR) may change latency → the kernel sets
  `propose_latency` after the swap and the standard latency handshake (AGENTS.md)
  takes over. **No special case.**

---

## Bindings

```cpp
// Processor: read + capture, through Dsp_context (tiny_processor.h).
struct Buffer_io {
    std::function<const Prepared*(uint32_t /*addr*/, uint32_t /*key*/)> current = /*…*/;
    std::function<std::span<float>(uint32_t /*addr*/)>                  capture = /*…*/;
    std::function<void(uint32_t /*addr*/, uint32_t /*frames*/)>         commit  = /*…*/;
};
struct Dsp_context {
    // …existing…
    Buffer_io buffers{};   // NEW
};

// Editor: thin push/remove + status/overview read, through Edit_context (tiny_edit.h).
struct Edit_context {
    Action_queue::Actor   actions{};
    Format                format{};
    State_adapter::Actor  state_adapter{};
    Undo_history::Actor   undo_redo{};
    Buffer_actor          buffers{};   // NEW: push(addr[,key], source) / remove(addr,key)
};
```

- The editor **never sees `Prepared`** — only the per-slot `status` scalar (read
  each frame, like a meter) and the overview Block.
- **Undo stays in the editor.** `push` / `remove` are discrete undoable actions the
  editor records in the existing `Undo_history` and replays — the buffer system
  owns no undo machinery.

---

## Persistence wire format (per format)

The `source` variant is what serializes. `embed` writes bytes; `reference` writes
path + hash; `consolidate` is embed-on-export; `transient` writes nothing.
Backward compatible: no buffer section ⇒ slots stay empty ⇒ no `Set_buffer` events.

### Binary formats (CLAP, VST3) — appended section

Written **after** existing data (after editor state in CLAP; after params in the
VST3 **processor** stream). No header change — old state simply lacks the section.

```
uint32_t  buffers_magic   ('bufs')
uint32_t  num_entries
for each entry:
    uint32_t  key_length
    char[]    string_id          (key_length bytes)
    uint32_t  collection_key     (0 for single)
    uint8_t   source_kind        (file_ref | embedded | generated)
    uint64_t  payload_length
    uint8_t[] payload            (path+hash, or [enc]+bytes, or generated params)
```

Loading old state (no section): after reading current data, attempt to read
`buffers_magic`; CLAP `read()` returning 0 (EOF) or VST3 `readInt32u()` returning
false ⇒ no buffers. Four bytes that don't match the magic ⇒ warn + skip.

### Dictionary formats (AAX, AUv2, AUv3) — keyed entries

One entry per (slot, key) under `"tinyplug-buffer-<string_id>[-<key>]"`, plus a
count key for validation. Missing key on load ⇒ that slot stays empty.

For each format, on load the wrapper reads each source → `edit/proc.set_source(...)`
→ framework schedules `prepare_buffer` → publishes `Set_buffer`. The editor watches
`status` go `loading → ready`.

### VST3 specifics (where principle 2 pays off)

- The canonical `source` lives on the **processor** (where `getState` is), so
  **persistence never crosses COM.**
- Only two small things cross: (a) **acquire** — the editor (controller) sends a
  `Buffer_source` to the processor via `IMessage` binary attribute, usually just a
  `File_ref` path; (b) **status + overview** out — processor → controller via the
  meter/`IDataExchangeHandler` path already used by Blocks/meters.
- Large in-memory `Embedded` acquire (rare: drag-drop of bytes with no file) may
  need `IMessage` chunking. The common file-picker path sends a path string and
  never hits this.

This is strictly simpler than the asset-store doc's "store mirrored across COM."

---

## Walkthrough of the four canonical plug-ins

**Looper** — `origin=processor, persist=embed, single, max_frames=N`, no `prepare`.
- *Transport:* capture into preallocated double scratch on the RT thread; `commit`
  swaps live ↔ scratch pointers (zero alloc). Overview pushed as a Block.
- *State-save:* framework reads the committed buffer via `encode_buffer` (raw or
  off-thread FLAC) → `[len][bytes]` into the chunk. **Processor-origin ⇒ VST3
  trivial** (bytes where `getState` lives; only the overview crosses COM). Restore:
  bytes → install (raw, no prepare).

**Granular (dual origin)** — `origin=either, prepare=decode/resample, persist=embed,
single`.
- *Transport:* editor path pushes a `File_ref` (tiny cross) → `prepare_buffer`
  decodes off-thread → install; capture path is processor-local like the looper.
  **Both feed the same slot.** Live swap under playback exercises the retire queue
  (old grain source freed off-thread).
- *State-save:* whatever the processor holds — encoded-file `Embedded` or
  captured-raw `Embedded` — persisted origin-agnostically; the `Encoding` tag tells
  `prepare_buffer` how to re-derive.

**One-shot sampler** — `origin=editor, prepare=decode/resample, persist=embed (or
reference), single`.
- *Transport:* `buffers.push(addr, File_ref{path})` → tiny path crosses → processor
  `prepare_buffer` decodes off-thread → install. Trigger via param/MIDI. Overview
  Block.
- *State-save:* `embed` stores bytes; `reference` stores path+hash. Restore re-runs
  `prepare_buffer` (sample-rate-dependent — the reason we persist source, not PCM).

**Drum machine (collection)** — `arity=collection, origin=editor,
prepare=decode, persist=embed (or reference), max_entries=16`.
- *Transport:* `buffers.push(addr, pad_key, File_ref)` / `buffers.remove(addr,
  pad_key)` per pad; each crosses as a value, prepares off-thread, installs into a
  per-key handle; the audio thread reads `ctx.buffers.current(addr, pad_key)`.
  Per-pad overview = a keyed Block.
- *State-save:* iterate collection entries, persist each keyed source; restore
  delivers per entry → prepare → install. Pad add/remove undo lives in the editor's
  `Undo_history`.

---

## What is deliberately excluded (this is what keeps it à la carte)

- **No framework-owned store/hub, no COM-mirrored object.** Slots live on the
  processor; the editor is a thin push + read.
- **No undo machinery** in the buffer system — the editor records push/remove.
- **No overview machinery** — overviews are [Blocks](block-output.md).
- **No collection keying beyond an integer key** (+ optional small per-entry
  metadata the author interprets). Multisample key-range/velocity zones aren't in
  the canonical four, so we don't design for them now.
- **No generic non-audio blob system.** Sequences/patterns are params; record/stop/
  gains/triggers are params; UI bits are the `State_map`. The old `State_model`'s
  generic-blob role isn't needed by the canonical four and isn't built.
- **No small fixed-vector `copy` Table channel.** None of the four need it; a
  continuously-morphed wavetable would, but that's a params/future concern, not
  this system. (Open question below.)

---

## Relationship to the rest of the framework

| Concern | Owner |
|---|---|
| Scalar in (automatable) | `Param_model` |
| Scalar out | `Meter_model` |
| Vector out (transient, viz, **overviews**) | `Block_model` ([block-output.md](block-output.md)) |
| Large audio buffer in + capture + install + **persist** | **this plan** |
| Small typed UI key/values | editor `State_map` |
| Author async request/reply | `Plug_worker` |

`Block` and `Buffer` are siblings, opposite directions and opposite lifetimes
(Block = transient/high-rate out; Buffer = persisted/rare in). They are *used
together* (a buffer slot's overview is a Block) but neither owns the other.

---

## Staging

1. **September slice = a strict subset.** The looper is this system with
   `origin=processor, prepare=none, persist=embed, single`. The minimal vertical:
   preallocated double scratch + `commit` swap + `[len][bytes]` chunk-append **only
   in the formats that plug-in ships** (not all five), hand-rolling one slot rather
   than the declarative `Buffer_model`. It forward-maps onto the full system with
   nothing thrown away. Skip for now: the declarative model machinery, the
   `Buffer_source` variant, collections, editor-origin acquire, `prepare_buffer`.
2. **Full system second.** The declarative `Buffer_model`, the `Buffer_source`
   variant + persistence across all five formats, editor-origin acquire +
   `prepare_buffer` + install/retire, collections — when committing to ship a
   sampler/granular/drum plug-in with a clean author experience.

Recommended order relative to other roadmap work (from the synthesis session):
`host-initiated-param-changes` → `midi-support` → `block-output` → this plan.
Build the outbound/overview primitive before the buffer system that draws with it.

---

## Open questions

- **`encode_buffer` vs. capture-encodes-on-commit.** Should captured content be
  encoded back to a `source` lazily at save (`encode_buffer`), or should `commit`
  publish an already-encoded source so persistence never calls back into the
  processor? The latter keeps `getState` a pure read; the former avoids encoding
  audio that is never saved.
- **Keep a small `copy` vector-in channel at all?** A morphing wavetable wants
  small/frequent value-copy inbound, which this system (rare/large/swap) serves
  awkwardly. Resurrect a minimal Table `copy` policy, or push that to params/future?
- **Compression codec + where it lives** (FLAC/zlib — a `tiny_deps` dependency?).
- **`reference` portability** inherits the classic "missing media" problem;
  `consolidate` (embed-on-export) is the escape hatch.
- **Collection metadata shape** if/when multisample zones are in scope.

---

## Verification (when built)

1. **Zero-buffer plug-in** compiles with no size/behaviour change (`if constexpr`
   branches gone).
2. **Sampler round-trip** — drop a file, save the session in each format, reload,
   confirm audio restored; confirm `prepare_buffer` runs off-thread and the audio
   thread never allocates (instrument the retire queue).
3. **Looper capture/persist** — record, save, reload, confirm playback; confirm no
   audio-thread allocation at capture/commit/save.
4. **Granular dual-origin** — feed the same slot from a file and from capture;
   confirm both persist and restore; confirm glitch-free live swap (TSan on
   install/retire).
5. **Drum machine collection** — load several pads, remove one, save/reload, confirm
   per-pad restore and undo.
6. **Convolution latency** — load an IR, confirm `propose_latency` fires and the
   host latency handshake completes.
7. **Old-state compat** — load a pre-feature session into the new build; buffers
   absent, params/editor intact, no crash.
8. **Format validators** (auval / pluginval / clap-validator) pass with buffers in
   the state round-trip.

---

## Design history (posterity)

Kept so the reasoning isn't lost when the three superseded docs are deleted.

- **Monolith → à la carte.** `asset-store.md` originally framed one `Asset_store`
  lifecycle (acquire→prepare→publish→render→persist→restore) that "subsumed"
  State/Block/Table via a **framework-owned hub mirrored across the VST3 COM
  boundary**. We flipped this: independent opt-in primitives that ship and stand
  alone, no hub. This buffer system is the in/persist/install primitive; Block is
  the out primitive; params/`State_map`/worker are untouched.
- **The value-semantics constraint killed the hub.** Because the VST3 controller
  and processor are distinct COM components, *no transport between
  editor/processor/worker can be pointer-based* — it must be value/bytes.
  Pointer-swap survives only as the intra-processor install. That, plus "the
  processor owns the canonical source," is what let the hub dissolve: persistence,
  overview, and re-install all became processor-local and origin-agnostic.
- **Table absorbed, not kept separate.** The old `Table_model` (fixed-size float
  SPSC copy queue) is the wrong mechanism for megabyte buffers. Its inbound role is
  generalized here into `Buffer_source` in + off-thread prepare + atomic
  pointer-swap install + deferred retire. A small `copy` Table for morphing
  wavetables is an open question, not a commitment.
- **State_model absorbed for audio.** "Persist a named blob" becomes the buffer
  slot's `source` serializer. The generic non-audio blob model isn't built —
  sequences are params, UI state is the `State_map`.
- **Origin lines up with difficulty.** Processor-origin (looper) needs only persist
  + install and is the easy VST3 case (the September slice). Editor-origin
  (sampler/drum) adds a one-time source cross + off-thread prepare. Granular proves
  one slot can take both.
