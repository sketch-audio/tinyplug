# Plan: Block output — processor → editor vector transport

> Status: **design.** Split out of the former `block-table-io.md`. The "Table"
> (editor → processor) half of that doc has been folded into
> [buffer-system.md](buffer-system.md); this plan is the outbound-vector half on
> its own, because it is an independent primitive with a real standalone purpose
> (visualization) and a different direction/lifetime from the buffer system.

## Context

tinyplug transports **scalar values** between editor and processor today:
parameters (editor → processor) and meters (processor → editor). Visualization
needs **vector transport** the other way — contiguous blocks of floats, lock-free
and thread-safe, pushed from the audio thread (or an off-thread job) and drained
by the immediate-mode editor.

This is the outbound, transient, high-rate primitive. It is the counterpart to
meters (scalar out) the way the buffer system is the counterpart to params
(persisted/installed content in):

| | Editor → Processor | Processor → Editor |
|---|---|---|
| **Scalar** | `Param_model` | `Meter_model` |
| **Vector / buffer** | `Buffer` ([buffer-system.md](buffer-system.md)) | **`Block_model` (this plan)** |

Uses: FFT bins (spectrum), oscilloscope snapshots, envelope/transfer curves, and
— importantly — **the waveform/peaks overview that every buffer-system plug-in
draws** (looper, granular, sampler, drum machine). Overviews are deliberately a
Block, not part of the buffer system: the buffer system owns content + transport
+ persistence; the editor's picture of it is an ordinary Block. The two compose.

All Block transport is **value semantics** (the editor copies the block out of the
channel), so it is correct across the VST3 controller/processor COM boundary.

---

## 1. Declarative model (`shared/tinyplug/tiny_blocks.h` — new)

Mirrors `Some_meter_model` exactly.

```cpp
// How the editor receives block updates.
enum class Block_policy : uint32_t {
    snapshot,   // Editor always gets the latest complete block (triple-buffer).
    stream      // Editor pops a FIFO of blocks (time-series: oscilloscope).
};

struct Block_spec {
    uint32_t     address{};
    uint32_t     num_elements{};     // floats per block
    Block_policy policy{};
    uint32_t     queue_depth{2};     // stream policy: FIFO depth
};

template<typename T>
concept Some_block_model = requires {
    typename T::Block_address;
    requires Enum<typename T::Block_address>;
    requires std::same_as<std::underlying_type_t<typename T::Block_address>, uint32_t>;
    { T::make_specs() } -> std::same_as<std::vector<Block_spec>>;
};

template<Some_block_model User_model>
class Block_infos {
public:
    static constexpr auto num_blocks = enum_raw(User_model::Block_address::num_blocks);
    static auto block_specs() -> const std::vector<Block_spec>&;
    static auto block_spec(uint32_t address) -> const Block_spec&;
};
```

Plug-in author view (`models/block_model.h`):

```cpp
struct Block_model {
    enum class Block_address : uint32_t {
        spectrum = 0,   // FFT bins
        waveform,       // oscilloscope / overview
        num_blocks
    };

    static auto make_specs() -> std::vector<Block_spec> {
        using enum Block_address;
        return {
            {enum_raw(spectrum), 1024, Block_policy::snapshot},
            {enum_raw(waveform), 512,  Block_policy::stream, /*queue_depth=*/4},
        };
    }
};
static_assert(Some_block_model<Block_model>);
```

Default empty model ships `num_blocks = 0` + empty `make_specs()` → every
`if constexpr (num_blocks > 0)` branch compiles away (same backward-compat
strategy as params/meters/state).

---

## 2. Transport: `Block_channel` (`tiny_blocks.h`)

**Snapshot policy** — triple-buffer (zero-copy on the producer side, pointer swap):
- Producer writes directly into the back buffer via `write_buffer()`, then
  `publish()` atomically swaps the "latest" index.
- Editor calls `read()` to atomically claim the latest published buffer.
- Three pre-allocated `std::vector<float>` of size `num_elements`. One atomic
  state word encodes read/write/spare indices.

**Stream policy** — SPSC ring of pre-allocated blocks:
- `queue_depth` pre-allocated blocks in a circular buffer.
- Producer pushes (may fail if full — dropped, acceptable for visualization).
- Editor pops one or more blocks per frame.

```cpp
class Block_channel {
public:
    explicit Block_channel(const Block_spec& spec);

    // Producer side (audio thread OR an off-thread job — see note below).
    auto write_buffer() -> std::span<float>;
    auto publish() -> void;

    // UI thread.
    auto read() -> std::span<const float>;   // snapshot: latest; stream: next in FIFO
    auto drain() -> void;                     // stream: discard remaining queued blocks
};
```

**Design choice:** the triple-buffer avoids the multi-KB copy an overwrite-queue
would force per block — the producer writes in place and swaps a pointer.

**Off-thread producers (extension over the original block-table-io draft).** The
producer is normally the audio thread inside `process()`, but the channel must
also accept publishes from a **background job** — e.g. a freshly-decoded waveform
overview computed by the buffer system's `prepare_buffer` job
([buffer-system.md](buffer-system.md)). The snapshot triple-buffer already
tolerates a single producer regardless of which thread it is, as long as it is
*one* producer at a time per channel. Document this; the overview channel's
producer is the prepare thread, not the audio thread, and these never overlap for
a given slot.

---

## 3. `Dsp_context` change (`shared/tinyplug/tiny_processor.h`)

```cpp
// Processor writes visualization data for the editor.
struct Block_writer {
    std::function<std::span<float>(uint32_t /*address*/)> write_buffer =
        [](auto) { return std::span<float>{}; };
    std::function<void(uint32_t /*address*/)> publish =
        [](auto) {};
};

struct Dsp_context {
    // ...existing...
    Block_writer blocks{};   // NEW — processor → editor
};
```

The processor concept is unchanged; blocks are reached through `Dsp_context`:

```cpp
auto Plug_processor::process(Dsp_context& ctx) -> void
{
    if (auto buf = ctx.blocks.write_buffer(enum_raw(Block_address::spectrum)); !buf.empty()) {
        compute_fft(buf);
        ctx.blocks.publish(enum_raw(Block_address::spectrum));
    }
    // ... normal DSP ...
}
```

`std::function` keeps the processor decoupled from transport: in-process wrappers
bind to a `Block_channel`; VST3 binds to `IDataExchangeHandler`.

---

## 4. Editor side

`Ui_receiver` ([tiny_events.h](../shared/tinyplug/tiny_events.h)) gains:

```cpp
using Read_block = std::function<std::span<const float>(uint32_t /*address*/)>;
Read_block read_block = [](auto) { return std::span<const float>{}; };
```

`Processor_state` ([tiny_utils.h](../shared/tinyplug/tiny_utils.h)) gains a blocks
view, indexed by `Block_address`:

```cpp
struct Processor_state {
    std::span<const double> params{};
    std::span<const double> meters{};
    std::span<const std::span<const float>> blocks{};   // NEW
};
```

`run_frame()` ([tiny_view.h](../shared/tinyplug/tiny_view.h)) reads all block
channels via `_receiver.read_block(addr)` before `on_gui_draw`, assembling them
into `Processor_state::blocks`. The editor pulls
`Plugin_state::processor_state.blocks[addr]` — the same pull pattern as
params/meters.

---

## 5. Format wrapper integration

### 5.1 In-process formats (CLAP, AUv2, AUv3, AAX)

The framework's own channels, used directly.

```cpp
using User_blocks = Block_infos<Block_model>;
std::vector<Block_channel> _block_channels{};   // one per block spec
```

In `process()` bind the `Dsp_context`:

```cpp
context.blocks = {
    .write_buffer = [this](uint32_t a) { return _block_channels[a].write_buffer(); },
    .publish      = [this](uint32_t a) { _block_channels[a].publish(); },
};
```

In view creation bind the `Ui_receiver`:

```cpp
receiver.read_block = [this](uint32_t a) { return _block_channels[a].read(); };
```

### 5.2 VST3 (two-component)

`IDataExchangeHandler` (VST 3.7.9+) is purpose-built for this — real-time safe,
lock-free, crosses the COM boundary by value:

- `Vst3_processor`:
  - `setupProcessing()`: query host for `IDataExchangeHandler`; `openQueue(...)`
    per block spec.
  - `process()`: `lockBlock()` → fill → `freeBlock(..., sendToController=true)`.
  - `setActive(false)`: `closeQueue()`.
- `Vst3_controller`:
  - implement `IDataExchangeReceiver`; `onDataExchangeBlocksReceived()` memcpys
    received data into a local `Block_channel` the editor reads.

**Fallback** (hosts without `IDataExchangeHandler`): `IMessage` with `setBinary()`
— processor stages blocks into a lock-free queue, a deferred callback ships them.
Lossy under load, acceptable for visualization.

### 5.3 Compatibility matrix

| Feature | VST3 | CLAP | AUv2 | AUv3 | AAX |
|---|---|---|---|---|---|
| Blocks (proc → editor) | `IDataExchangeHandler` / `IMessage` fallback | `Block_channel` | `Block_channel` | `Block_channel` | `Block_channel` |

---

## 6. Implementation sequence

1. `tiny_blocks.h` — `Block_spec`, `Block_policy`, `Some_block_model`,
   `Block_infos`, `Block_channel`.
2. `Block_writer` on `Dsp_context` (`tiny_processor.h`).
3. `read_block` on `Ui_receiver` (`tiny_events.h`); `blocks` on `Processor_state`
   (`tiny_utils.h`); populate in `run_frame()` (`tiny_view.h`).
4. Default empty `block_model.h` for existing plug-ins; confirm they compile
   unchanged.
5. CLAP wrapper (reference), then AUv2 / AUv3 / AAX (same pattern).
6. VST3 `IDataExchangeHandler` + `IMessage` fallback.
7. Demo: spectrum analyzer (snapshot) + oscilloscope (stream).

---

## 7. Open decisions

1. **Element type:** `float` only for now (covers audio); a `double` variant can
   be added later.
2. **Variable-sized blocks:** fixed `num_elements` per spec; a plug-in needing
   variable sizes declares the max and uses a sub-range (or we add an
   `actual_size` header if it becomes common).
3. **Stream drain:** auto-drain stale blocks at the start of `run_frame()`, or let
   the editor call `drain()` explicitly?

---

## Verification

- All existing plug-ins compile unchanged with the empty block model.
- Spectrum demo: processor FFT → editor draws — smooth in CLAP and VST3.
- VST3 tested with a host that supports `IDataExchangeHandler` (Cubase 13+) and
  one that does not (fallback path).
- Stress: no audio glitches when the stream queue is full (graceful drop).
- TSan: triple-buffer correctness (no torn reads, no data races), including an
  off-thread overview producer.

---

## Key files

| File | Change |
|---|---|
| `shared/tinyplug/tiny_blocks.h` | **New** — model, infos, `Block_channel` |
| `shared/tinyplug/tiny_processor.h` | Add `Block_writer` to `Dsp_context` |
| `shared/tinyplug/tiny_events.h` | Add `read_block` to `Ui_receiver` |
| `shared/tinyplug/tiny_utils.h` | Add `blocks` to `Processor_state` |
| `shared/tinyplug/tiny_view.h` | Populate block snapshots in `run_frame()` |
| `formats/clap/source/clap_plugin.h` | Channels + wiring (reference) |
| `formats/vst3/source/vst3_processor.h` | `IDataExchangeHandler` |
| `formats/vst3/source/vst3_controller.h` | `IDataExchangeReceiver` |
| `formats/auv2|auv3|aax/...` | Channels + wiring |
| `plugins/*/source/models/block_model.h` | **New** empty default per plug-in |
