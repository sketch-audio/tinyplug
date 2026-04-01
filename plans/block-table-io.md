# Plan: Add Block and Table I/O Between Editor and Processor

## Context

tinyplug currently transports **scalar values** between editor and processor: parameters (editor→processor) and meters (processor→editor). Visualization and data-loading use cases need **vector transport** — contiguous blocks of floats, lock-free and thread-safe.

The design mirrors the existing param/meter symmetry with two new models:

| | Editor → Processor | Processor → Editor |
|---|---|---|
| **Scalar** | `Param_model` | `Meter_model` |
| **Vector** | `Table_model` | `Block_model` |

- **Block**: processor sends visualization data to editor (FFT bins, oscilloscope snapshots, envelope curves)
- **Table**: editor sends loaded data to processor (wavetables, IRs, audio samples)

---

## 1. Block Model — Processor → Editor (`shared/tinyplug/tiny_blocks.h` — new file)

Mirrors `Some_meter_model` exactly.

### Block_spec

```cpp
// How the editor receives block updates.
enum class Block_policy : uint32_t {
    snapshot,   // Editor always gets the latest complete snapshot (triple-buffer).
    stream      // Editor pops a FIFO of blocks (time-series: oscilloscope).
};

struct Block_spec {
    uint32_t address{};
    uint32_t num_elements{};         // Number of floats per block.
    Block_policy policy{};
    uint32_t queue_depth{2};         // For stream policy: FIFO depth.
};
```

### Concept

```cpp
template<typename T>
concept Some_block_model = requires {
    typename T::Block_address;
    requires Enum<typename T::Block_address>;
    requires std::same_as<std::underlying_type_t<typename T::Block_address>, uint32_t>;
    { T::make_specs() } -> std::same_as<std::vector<Block_spec>>;
};
```

### Plugin author usage (`models/block_model.h`)

```cpp
struct Block_model {
    enum class Block_address : uint32_t {
        spectrum = 0,     // FFT bins
        waveform,         // oscilloscope snapshot
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

### Block_infos

```cpp
template<Some_block_model User_model>
class Block_infos {
public:
    static constexpr auto num_blocks = enum_raw(User_model::Block_address::num_blocks);
    static auto block_specs() -> const std::vector<Block_spec>&;
    static auto block_spec(uint32_t address) -> const Block_spec&;
};
```

### Default empty model

```cpp
struct Block_model {
    enum class Block_address : uint32_t { num_blocks = 0 };
    static auto make_specs() -> std::vector<Block_spec> { return {}; }
};
```

---

## 2. Table Model — Editor → Processor (`shared/tinyplug/tiny_tables.h` — new file)

Same concept pattern for the opposite direction.

### Table_spec

```cpp
struct Table_spec {
    uint32_t address{};
    uint32_t num_elements{};         // Number of floats per table.
    uint32_t queue_depth{2};         // FIFO depth (usually small: 2-4).
};
```

### Concept

```cpp
template<typename T>
concept Some_table_model = requires {
    typename T::Table_address;
    requires Enum<typename T::Table_address>;
    requires std::same_as<std::underlying_type_t<typename T::Table_address>, uint32_t>;
    { T::make_specs() } -> std::same_as<std::vector<Table_spec>>;
};
```

### Plugin author usage (`models/table_model.h`)

```cpp
struct Table_model {
    enum class Table_address : uint32_t {
        wavetable = 0,    // user-loaded wavetable
        ir_data,          // impulse response
        num_tables
    };

    static auto make_specs() -> std::vector<Table_spec> {
        using enum Table_address;
        return {
            {enum_raw(wavetable), 2048, /*queue_depth=*/2},
            {enum_raw(ir_data),   4096, /*queue_depth=*/2},
        };
    }
};
static_assert(Some_table_model<Table_model>);
```

### Table_infos

```cpp
template<Some_table_model User_model>
class Table_infos {
public:
    static constexpr auto num_tables = enum_raw(User_model::Table_address::num_tables);
    static auto table_specs() -> const std::vector<Table_spec>&;
    static auto table_spec(uint32_t address) -> const Table_spec&;
};
```

### Default empty model

```cpp
struct Table_model {
    enum class Table_address : uint32_t { num_tables = 0 };
    static auto make_specs() -> std::vector<Table_spec> { return {}; }
};
```

---

## 3. Lock-Free Transport Channels

### Block_channel — processor → editor (`tiny_blocks.h`)

**Snapshot policy** — triple-buffer (zero-copy, pointer swap):
- Audio thread writes directly into back buffer via `write_buffer()`, calls `publish()` to atomically swap the "latest" index.
- Editor calls `read()` to atomically claim the latest published buffer.
- Three pre-allocated `std::vector<float>` of size `num_elements`. Atomic state word encodes read/write/spare indices.

**Stream policy** — SPSC queue of pre-allocated blocks:
- `queue_depth` pre-allocated blocks in a circular buffer.
- Audio thread pushes (may fail if full — dropped, acceptable for visualization).
- Editor pops one or more blocks per frame.

```cpp
class Block_channel {
public:
    explicit Block_channel(const Block_spec& spec);

    // Audio thread.
    auto write_buffer() -> std::span<float>;
    auto publish() -> void;

    // UI thread.
    auto read() -> std::span<const float>;   // Snapshot: latest. Stream: next in FIFO.
    auto drain() -> void;                     // Stream: discard remaining queued blocks.
};
```

**Design choice**: Triple-buffer avoids the 4KB+ copy that `Overwrite_queue` would require per block. The audio thread writes in-place and swaps a pointer.

### Table_channel — editor → processor (`tiny_tables.h`)

SPSC queue of pre-allocated blocks. Editor copies data in, processor reads out.

```cpp
class Table_channel {
public:
    explicit Table_channel(const Table_spec& spec);

    // UI thread.
    auto push(std::span<const float> data) -> bool;  // false if queue full.

    // Audio thread.
    auto pop() -> std::span<const float>;  // Empty span if nothing available.
};
```

Pre-allocated: `queue_depth` slots of `num_elements` floats. No runtime allocations.

---

## 4. Dsp_context Changes (`shared/tinyplug/tiny_processor.h`)

Two separate I/O structs, one per direction:

```cpp
// Processor writes visualization data for the editor.
struct Block_writer {
    std::function<std::span<float>(uint32_t /*address*/)> write_buffer =
        [](auto) { return std::span<float>{}; };
    std::function<void(uint32_t /*address*/)> publish =
        [](auto) {};
};

// Processor reads data loaded by the editor.
struct Table_reader {
    std::function<std::span<const float>(uint32_t /*address*/)> pop =
        [](auto) { return std::span<const float>{}; };
};

struct Dsp_context {
    Musical_context musical_context{};
    std::span<const float*> ibuffers{};
    std::span<const float*> sbuffers{};
    std::span<float*> obuffers{};
    size_t num_frames{};
    std::span<float> meters{};
    std::optional<uint32_t> propose_latency{};
    Block_writer blocks{};    // NEW — processor → editor
    Table_reader tables{};    // NEW — editor → processor
};
```

**Processor concept unchanged.** Blocks and tables accessed through `Dsp_context`.

Processor usage:

```cpp
auto Plug_processor::process(Dsp_context& ctx) -> void
{
    // Write FFT data for editor visualization.
    if (auto buf = ctx.blocks.write_buffer(enum_raw(Block_address::spectrum)); !buf.empty()) {
        compute_fft(buf);
        ctx.blocks.publish(enum_raw(Block_address::spectrum));
    }

    // Read wavetable data loaded by user in the editor.
    if (auto wt = ctx.tables.pop(enum_raw(Table_address::wavetable)); !wt.empty()) {
        load_wavetable(wt);
    }

    // ... normal DSP ...
}
```

**Why `std::function` callbacks**: Keeps processor decoupled from transport. CLAP wrapper binds to `Block_channel`/`Table_channel` directly; VST3 wrapper binds to `IDataExchangeHandler` calls. Same processor code, different transport.

---

## 5. Editor Interface Changes

### Ui_receiver extension (`tiny_events.h`)

```cpp
struct Ui_receiver {
    // ... existing ...
    Get_param get_param = [](auto) { return 0; };
    Pop_meter pop_meter = [](auto&) { return false; };
    Action_handler action_handler = [](auto&) {};

    // NEW: read block data from processor (visualization).
    using Read_block = std::function<std::span<const float>(uint32_t /*address*/)>;
    Read_block read_block = [](auto) { return std::span<const float>{}; };

    // NEW: push table data to processor (sample loading).
    using Push_table = std::function<bool(uint32_t /*address*/, std::span<const float> /*data*/)>;
    Push_table push_table = [](auto, auto) { return false; };
};
```

### Processor_state extension (`tiny_utils.h`)

```cpp
struct Processor_state {
    std::span<const double> params{};
    std::span<const double> meters{};
    std::span<const std::span<const float>> blocks{};  // NEW: indexed by Block_address
};
```

### run_frame() changes (`tiny_view.h`)

Before `on_gui_draw`, `run_frame()` reads all block channels via `_receiver.read_block(addr)` and assembles them into `Processor_state::blocks`. Editor reads from `Plugin_state::processor_state.blocks[addr]` — same pull pattern as params/meters.

For tables, editor calls `_receiver.push_table(address, data)` directly — a push action like `action_handler`. Tables bypass the `User_action` system since they are not undoable and too large for the action queue.

---

## 6. Format Wrapper Integration

### 6.1 In-Process Formats (CLAP, AUv2, AUv3, AAX)

Framework's own channels used directly.

**Wrapper members** (e.g., in `Clap_plugin`):

```cpp
using User_blocks = Block_infos<Block_model>;
using User_tables = Table_infos<Table_model>;
std::vector<Block_channel> _block_channels{};    // One per block spec.
std::vector<Table_channel> _table_channels{};    // One per table spec.
```

**In process()** — bind `Dsp_context`:

```cpp
context.blocks = {
    .write_buffer = [this](uint32_t a) { return _block_channels[a].write_buffer(); },
    .publish = [this](uint32_t a) { _block_channels[a].publish(); },
};
context.tables = {
    .pop = [this](uint32_t a) { return _table_channels[a].pop(); },
};
```

**In view creation** — bind `Ui_receiver`:

```cpp
receiver.read_block = [this](uint32_t a) { return _block_channels[a].read(); };
receiver.push_table = [this](uint32_t a, std::span<const float> d) { return _table_channels[a].push(d); };
```

Same wiring for AUv2, AUv3, AAX.

### 6.2 VST3 (Two-Component)

**Blocks (processor → controller): `IDataExchangeHandler` (VST 3.7.9+)**

Purpose-built for this exact use case — real-time safe, lock-free.

- `Vst3_processor`:
  - In `setupProcessing()`: query host for `IDataExchangeHandler`. Open one queue per block spec via `openQueue(blockSize, numBlocks, ...)`.
  - In `process()`: `lockBlock()` → fill data → `freeBlock(..., sendToController=true)`.
  - In `setActive(false)`: `closeQueue()`.

- `Vst3_controller`:
  - Implement `IDataExchangeReceiver`.
  - `onDataExchangeBlocksReceived()`: memcpy received data into local `Block_channel` for editor to read.

**Fallback** (hosts without `IDataExchangeHandler`): `IMessage` with `setBinary()`. Processor stages data into a lock-free queue; deferred callback sends messages. Lossy under load — acceptable for visualization.

**Tables (controller → processor): `IMessage` / `IConnectionPoint`**

- `Vst3_controller`: `push_table()` creates `IMessage`, attaches data via `IAttributeList::setBinary()`, sends via `sendMessage()`.
- `Vst3_processor`: receives in `notify()`, copies into `Table_channel`. Processor pops during `process()`.

Both already inherit from `ComponentBase` (implements `IConnectionPoint`). Just override `notify()` and use `sendMessage()`.

### 6.3 Compatibility Matrix

| Feature | VST3 | CLAP | AUv2 | AUv3 | AAX |
|---|---|---|---|---|---|
| Blocks (proc→editor) | IDataExchangeHandler / IMessage fallback | Block_channel | Block_channel | Block_channel | Block_channel |
| Tables (editor→proc) | IMessage | Table_channel | Table_channel | Table_channel | Table_channel |

---

## 7. Implementation Sequence

### Phase 1: Core types (no format changes)
1. Create `shared/tinyplug/tiny_blocks.h` — `Block_spec`, `Block_policy`, `Some_block_model`, `Block_infos`, `Block_channel`
2. Create `shared/tinyplug/tiny_tables.h` — `Table_spec`, `Some_table_model`, `Table_infos`, `Table_channel`
3. Add `Block_writer` and `Table_reader` structs to `Dsp_context` in `shared/tinyplug/tiny_processor.h`
4. Extend `Ui_receiver` in `shared/tinyplug/tiny_events.h` with `read_block` and `push_table`
5. Extend `Processor_state` in `shared/tinyplug/tiny_utils.h` with `blocks` span
6. Update `run_frame()` in `shared/tinyplug/tiny_view.h` to populate block snapshots
7. Include new headers in main tinyplug header

### Phase 2: Empty default models for existing plugins
8. Add default empty `block_model.h` and `table_model.h` to each existing plugin
9. Verify all existing plugins compile unchanged

### Phase 3: CLAP wrapper (reference implementation)
10. Add `Block_infos`, `Table_infos`, channels to `formats/clap/source/clap_plugin.h`
11. Wire `Dsp_context` in process loop
12. Wire `Ui_receiver` in view creation

### Phase 4: AUv2, AUv3, AAX wrappers (same pattern)
13. `formats/auv2/source/auv2_effect.h`
14. AUv3 DSPKernel
15. `formats/aax/source/aax_parameters.cpp`

### Phase 5: VST3 wrapper
16. `Vst3_processor`: `IDataExchangeHandler` for blocks — query, openQueue, lock/free in process
17. `Vst3_processor`: `notify()` override for tables from controller
18. `Vst3_controller`: `IDataExchangeReceiver` for blocks
19. `Vst3_controller`: `IMessage` send for tables
20. Fallback for hosts without `IDataExchangeHandler`

### Phase 6: Demo and testing
21. Spectrum analyzer demo (blocks out, snapshot policy)
22. Wavetable loader demo (tables in)
23. Test across formats in DAWs

---

## 8. Open Design Decisions

1. **Element type**: `float` only for now. Covers all audio use cases. `double` variant can be added later if needed.

2. **Variable-sized blocks/tables**: Fixed `num_elements` per spec. If a plugin needs variable sizes (e.g., different FFT sizes), declare the max and use a sub-range. Could add `actual_size` header if this becomes common.

3. **Stream drain behavior**: When editor can't keep up, should framework auto-drain old blocks at start of `run_frame()`? Or let the editor decide via explicit `drain()`?

4. **Table consumption acknowledgment**: Should the framework notify the editor when processor has consumed a table? Currently no feedback. For sample loading the processor applies data immediately, so ambiguity is minimal.

5. **VST3 `IDataExchangeHandler` fallback**: IMessage (lossy, goes through host) vs shared-memory (faster, assumes same process). IMessage is safer and more compatible.

---

## Verification

- All existing plugins compile unchanged with empty block/table models
- Spectrum demo: processor FFT → editor draws — verify smooth in CLAP and VST3
- Wavetable demo: editor loads .wav → processor receives — verify data arrives
- VST3: test with host supporting `IDataExchangeHandler` (Cubase 13+) and one without
- Stress test: no audio glitches when block queue is full (graceful drop)
- TSan: verify triple-buffer correctness (no torn reads, no data races)

---

## Key Files

| File | Changes |
|------|---------|
| `shared/tinyplug/tiny_blocks.h` | **New** — Block_spec, Some_block_model, Block_infos, Block_channel |
| `shared/tinyplug/tiny_tables.h` | **New** — Table_spec, Some_table_model, Table_infos, Table_channel |
| `shared/tinyplug/tiny_processor.h` | Add Block_writer, Table_reader, extend Dsp_context |
| `shared/tinyplug/tiny_events.h` | Add read_block, push_table to Ui_receiver |
| `shared/tinyplug/tiny_utils.h` | Add blocks to Processor_state |
| `shared/tinyplug/tiny_view.h` | Update run_frame() to populate block snapshots |
| `formats/clap/source/clap_plugin.h` | Block_infos, Table_infos, channels, wiring |
| `formats/vst3/source/vst3_processor.h` | IDataExchangeHandler, IMessage notify for tables |
| `formats/vst3/source/vst3_controller.h` | IDataExchangeReceiver, IMessage send for tables |
| `formats/auv2/source/auv2_effect.h` | Channels and wiring |
| `formats/aax/source/aax_parameters.cpp` | Channels and wiring |
| `plugins/*/source/models/block_model.h` | **New** per plugin (empty default for existing) |
| `plugins/*/source/models/table_model.h` | **New** per plugin (empty default for existing) |
