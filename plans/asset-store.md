# Plan: Asset_store — Managed large-buffer resources (sampler / convolution / granular / looper)

> Status: **design / exploratory.** Captures an agreed direction to revisit
> later. Supersedes how [state-model.md](state-model.md) and
> [block-table-io.md](block-table-io.md) would be exposed (see
> "Relationship to State/Block/Table" below). Not yet scheduled for build.

## Context

A class of plug-ins is built around **one or more large audio buffers that the
DSP consumes**: samplers (PCM keymaps), convolution reverbs / cab sims (impulse
responses), granular engines (grain sources), loopers (captured audio). tinyplug
today has a data model for everything *except* this: params are scalar and
automatable, meters are scalar streams out, editor state is small typed key/value
([state_adapter](../shared/tinyplug/state_adapter.hpp)), and the worker carries
trivially-copyable messages ([tiny_worker.h](../shared/tinyplug/tiny_worker.h)).
There is no first-class notion of a **large, managed, off-thread-loaded resource**.

These plug-ins all repeat the same six-step lifecycle:

1. **Acquire** — a file is dropped/picked in the editor, or content is captured
   from the audio stream (looper), or generated.
2. **Prepare** — decode/resample/normalize and derive a DSP-ready form: build a
   keymap (sampler), FFT-partition the IR (convolution), window grains
   (granular). **Non-realtime.**
3. **Publish** — hand the prepared form to the audio thread with no allocation
   or locks, swappable live.
4. **Render** — the audio thread reads the current resource per block.
5. **Persist** — survive session save; optionally portable.
6. **Restore** — rebuild the prepared form from persisted source.

`Asset_store` factors this lifecycle out once so an author writes only the
DSP-specific `prepare` and render code.

## Three representations of one asset (keep distinct)

- **`Asset_source`** — the canonical, *persisted* thing. A variant:
  `Embedded{bytes, encoding}`, `File_ref{path, sha256}`, `Generated{params}`.
  Small. This is what serializes.
- **`Prepared`** — the RT-ready *derived* form. **Author-defined per slot**
  (decoded PCM keymap, partitioned IR, grain table). Immutable once published.
  Often sample-rate / block-size dependent — which is *why* we persist `source`
  and re-derive `Prepared` on load rather than storing decoded data.
- **`Overview`** — lightweight UI representation (waveform peaks, length,
  channels). Derived at prepare time so the editor draws without holding full PCM.

## Declarative interface (plug-in author view)

`models/asset_model.h` (new template file), parallel to `param_model.h`:

```cpp
struct Asset_model {
    enum class Asset_address : uint32_t { sample_pool = 0, num_assets };
    static auto make_specs() -> std::vector<tiny::Asset_spec>;
};
static_assert(tiny::Some_asset_model<Asset_model>);
```

```cpp
enum class Asset_kind   { audio_buffer, opaque_bytes };
enum class Asset_arity  { single, collection };          // collection = keyed/indexed
enum class Asset_origin { editor, processor };           // file/UI-loaded vs captured
enum class Persist_policy { reference, embed, consolidate };

struct Asset_spec {
    uint32_t      address{};
    std::string   string_id{};
    std::string   name{};
    Asset_kind    kind{Asset_kind::audio_buffer};
    Asset_arity   arity{Asset_arity::single};
    Asset_origin  origin{Asset_origin::editor};
    Persist_policy policy{Persist_policy::reference};
};
```

Default template ships `num_assets` only + empty `make_specs()` → zero overhead,
all `if constexpr (num_assets > 0)` branches compile away (mirrors the
`State_model` backward-compat strategy).

Author-supplied processor code (only when `num_assets > 0`, enforced by
`static_assert` + a concept like `Has_assets<Plug_processor>`):

```cpp
// The DSP-ready type + how to build it off-thread. `Prepared` is the author's type.
auto prepare_asset(uint32_t addr, std::span<const uint8_t> source,
                   const Prepare_context& ctx) -> Prepared;   // ctx: sample_rate, max_block_size, decoder
// Render reads the current handle (see processor integration).
```

## Core: `Asset_store` (framework-owned hub)

A framework object owned by **neither** editor nor processor — both hold thin
handles. This single ownership choice dissolves the "editor loads it / processor
renders it / who serializes it?" tension. Per slot (or per collection entry):

```
source     : Asset_source                               // canonical, for persistence
prepared   : atomic<shared_ptr<const Prepared>>          // RT-published
overview   : Overview                                    // for the UI
status     : empty | loading | ready | error
epoch      : uint32_t                                    // bumped on each publish
```

The store plays three roles: **scheduler** (kicks off prepare), **publisher**
(RT-safe handoff to the processor), **serializer** (owns persistence).

## Editor integration

Add an `Asset_store::Actor` to `Edit_context`
([tiny_edit.h](../shared/tinyplug/tiny_edit.h)), beside `actions` / `state_adapter`
/ `undo_redo`:

```cpp
struct Edit_context {
    Action_queue::Actor   actions{};
    Format                format{};
    State_adapter::Actor  state_adapter{};
    Undo_history::Actor   undo_redo{};
    Asset_store::Actor    assets{};   // new
};
```

- **Acquire:** on drag-drop / platform file-picker result, editor calls
  `_edit.assets.set_source(addr, File_ref{path})` (single) or
  `.add(addr, key, source)` / `.remove(addr, key)` (collection). Store sets
  `status = loading`, schedules prepare.
- **Observe:** the editor is immediate-mode, so it *reads* asset state each frame.
  Extend `Plugin_state` ([tiny_view.h](../shared/tinyplug/tiny_view.h)) with an
  asset view next to `processor_state`:

```cpp
struct Plugin_state {
    Processor_state processor_state{};
    Asset_state     asset_state{};   // status + overview per slot, read-only
    View_context    view_context{};
};
```

  `on_gui_draw` draws a spinner while `loading`, the waveform from `overview`
  when `ready`, an error glyph on `error`. Optional opt-in `on_asset_ready`
  push callback (concept-detected like worker replies / `on_host_event`).
- **Undo:** `set_source` / `clear` / `replace` / `trim` are discrete undoable
  ops. Store records old-source → new-source and pushes a step into
  `Undo_history` ([undo_history.hpp](../shared/tinyplug/undo_history.hpp)); undo
  replays the swap (coarser change unit than a param).

## Worker / prepare integration

- **Prepare runs on the framework background executor** —
  `Task_manager` background queue
  ([task_manager.hpp](../shared/tinyplug/task_manager.hpp)) — *not* the user
  worker channels (which are trivially-copyable only and can't move big buffers).
  Store calls `prepare_asset(addr, bytes, Prepare_context{sample_rate,
  max_block_size, decoder})`. `max_block_size` matters: convolution partitioning
  and sampler buffering depend on it.
- **Relationship to `Plug_worker`:** the asset loader is a *specialized
  framework-managed worker for large immutable resources*, built on the same
  thread primitives but separate from the user's request/reply worker. The user
  worker stays for author-driven async needs (disk streaming, analysis,
  generation).
- **RT publish uses the existing event model.** When prepare finishes the store:
  (1) atomically stores the new `shared_ptr<const Prepared>`, bumps `epoch`;
  (2) enqueues a `Set_asset` onto the processor `Render_event` queue
  ([tiny_events.h](../shared/tinyplug/tiny_events.h)):

```cpp
struct Set_asset { uint32_t address{}; std::shared_ptr<const Prepared> data{}; };
using Render_event = std::variant<Set_param, Ramp_param, Accepted_latency, Set_asset>;
```

  The processor receives it *between* `process` calls; the shared_ptr copy is
  RT-safe because allocation happened off-thread. Mirrors the planned
  `Load_state_item` and the meter queues.

## Processor integration

- The processor **reads, never owns or frees.** It holds an `Asset_reader`
  (bound at construction via `bind_assets`, the `bind_worker` pattern) or reads
  through `Dsp_context` ([tiny_processor.h](../shared/tinyplug/tiny_processor.h)):

```cpp
struct Dsp_context {
    // ...existing...
    Asset_reader assets{};   // assets.current(addr) -> const Prepared*
};
```

- On `handle_event(Set_asset)`: swap local handle, push the **old** handle onto a
  `retire` lock-free queue. Store drains `retire` on the background thread so
  freeing megabytes never happens on the audio thread (deferred reclamation).
- In `process`: `ctx.assets.current(addr)` returns the `const Prepared*` valid
  for that block.
- **Latency:** a new IR usually changes latency → kernel sets `propose_latency`
  after the swap; the standard latency-change handshake (AGENTS.md) takes over.
  No special case.

## Overall state-model integration

Asset **sources** become a third persistence surface alongside per-param scalars
and the editor `State_map`. The store is the serializer.

- **Native host state (session save):** each format's
  `getState`/`SaveChunk`/`fullState`/`getStateInformation` adds an asset-source
  section — reuse the appended binary-section wire format from
  [state-model.md](state-model.md) (`'sttm'`-style: magic, count, per-entry
  `string_id` + length + bytes), or per-format dict keys for AU/AAX. `embed`
  writes bytes; `reference` writes path + hash. Backward compatible: no asset
  section ⇒ store stays empty ⇒ slots empty.
- **Load:** `setState`/`RestoreState`/`SetChunk`/`setComponentState`/`stateLoad`
  reads sources → `store.set_source(...)` → store schedules prepare → publishes
  `Set_asset`. Editor sees `loading → ready`.
- **Portable preset (`State_adapter`):** follows `policy`. `reference` ⇒ a
  `states` entry in the JSON with path + hash (stays plain JSON; "missing-media"
  caveat). `embed` ⇒ a CBOR-via-nlohmann container (nlohmann is already a
  dependency and has a native binary type + `to_cbor`), emitted only when
  something is actually embedded — blob-less presets stay plain JSON.
- **VST3 split components:** acquire happens on the **controller**, but
  `getState`/render are on the **processor**. The source must cross
  controller→processor over `IMessage` (binary attribute), like the worker and
  `setComponentState` already cross ([vst3_messaging.h](../formats/vst3/source/vst3_messaging.h)).
  Processor side holds the canonical source (for `getState`) + runs prepare; the
  controller keeps `overview` for the UI. The store is effectively mirrored
  across the COM boundary, consistent with param-value mirroring. The other four
  formats have a single in-process store.

## Producer-side assets (looper) — bidirectional slot

A looper captures content **on the audio thread**; the processor is the source of
truth and the content must flow *out* for persistence. Modeled with
`origin = Asset_origin::processor` and `policy = embed` (captured audio has no
external file). Make the slot symmetric:

- **Playback** always reads the *current* `Prepared` (from a capture-commit or a
  restore) — one code path.
- **Record** writes into **preallocated** scratch (a looper must preallocate a
  max loop length at `reset(sr)`; the audio thread can't grow buffers). On commit,
  scratch is published as the new `Prepared`.
- **Capture path (outbound, processor → store):** reuse the outbound transport
  (the Block_model-shaped machinery) — either preallocated snapshot/triple-buffer
  or a streaming FIFO + off-thread accumulate (better for unbounded length).
  Store ends up holding the committed loop off-thread + an `Overview`.
- **Persist:** store serializes what it holds — **no audio-thread involvement at
  save time.** Always `embed`; may **compress off-thread** (e.g. FLAC) since
  serialization is already off the audio thread. (VST3 is *easier* here: content
  originates on the processor where `getState` lives — no source crossing, only
  `overview` goes to the controller.)
- **Restore (inbound):** identical to sampler — store → `Set_asset` → processor
  installs current `Prepared` (pointer swap, no copy).
- **Editor:** no file picker; reads `overview` to draw the waveform; record/stop
  are params/commands; clear/trim are undoable source edits.

This proves the outbound (Block-shaped) transport is needed beyond
visualization, and that the store is the single serializer regardless of origin.

## Relationship to State / Block / Table models

Two orthogonal axes — direction (in/out of RT) and lifecycle (persisted/transient):

- **`Block_model` stays as its own thing** — DSP→UI visualization (scopes,
  spectra), transient, high-rate. Opposite direction from Asset; its *machinery*
  is reused for the looper capture path, but its user-facing role is unaffected.
- **`Table_model` machinery is absorbed** — the `Set_asset` + retire transport
  *is* Table's snapshot policy generalized from float-vectors to arbitrary
  `Prepared`. A standalone Table may only survive for transient,
  never-persisted, streaming vectors (e.g. a continuously-morphed wavetable).
- **`State_model` is largely subsumed** — its job (persist named binary) becomes
  the store's asset-source serializer. A "state item" = an asset with identity
  prepare + embed policy.

Conclusion: **Asset is the headline abstraction; State and Table fold in as its
persistence and transport layers; Block stays separate.** Decide this layering
*before* building `State_model` as currently specced, or it gets built twice.

## Staging

1. **Primitive first:** off-thread `prepare` + RT-safe published-pointer handoff
   (`Set_asset` + retire) + the outbound capture transport. This alone makes
   sampler/convolution/granular/looper *buildable* (author DIYs persistence).
2. **Declarative `Asset_model` second:** the store, collections, policy-driven
   persistence, overview/peaks, editor `Asset_state` + undo — when committing to
   ship one of these plug-ins with a clean author experience.

## Limitations / open questions

- **Preallocation** is mandatory for producer-side (looper) and bounded for
  collections; max sizes need a declaration mechanism.
- **Collections** need a keying scheme (sampler zones: key range + velocity layer
  + root note) — design the metadata shape.
- **`reference` portability** inherits the classic "missing media" problem;
  `consolidate` (embed-on-export) is the escape hatch.
- **Undo granularity** for asset edits vs. param edits — coarse step is fine but
  interaction with the param undo stack needs definition.
- **Compression** (FLAC/zlib) policy + where the codec lives (a `tiny_deps`
  dependency?).
- **VST3 large-source `IMessage` chunking** — binary attributes may need
  splitting for very large embedded sources.

## Verification (when built)

1. **Zero-asset plug-in** compiles with no size/behavior change (`if constexpr`
   branches gone).
2. **Sampler round-trip** — load a multisample, save session in each format,
   reload, confirm keymap + audio restored; confirm prepare runs off-thread and
   the audio thread never allocates (instrument the retire queue).
3. **Convolution latency** — load an IR, confirm `propose_latency` fires and the
   host latency handshake completes.
4. **Looper capture/persist** — record a loop, save session, reload, confirm
   playback; confirm no audio-thread allocation at capture/commit/save.
5. **Old-state compat** — load pre-feature session into the new build; assets
   absent, params/editor intact, no crash.
6. **Format validators** (auval / pluginval / clap-validator) pass with assets in
   the state round-trip.
