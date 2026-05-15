# Plan: State_model — Per-plugin Extra State Serialization

## Context

tinyplug currently serializes two kinds of persistent data: **parameter values** (host-facing, automatable) and **editor state** (`State_map` — small typed key-value pairs for UI state). Neither is suited to large, blob-like data such as an audio loop, an impulse response, or a MIDI sequence that a plugin needs to save alongside its parameters.

`State_model` is a new, optional, declarative model (mirroring `Param_model` and `Meter_model`) that lets the plugin author register named state items. Each item is an opaque byte buffer. The framework serializes/deserializes the buffers in all five format wrappers and delivers them to the processor via the existing render-event queue.

Backward compatibility is preserved for all existing shipped plugins: if no `State_model` is present (or if the loaded state predates the feature), state items simply arrive as empty and the processor never receives a `Load_state_item` event.

---

## Declarative Interface (plugin-author view)

### `models/state_model.h` (template file — new)

```cpp
struct State_model {
    enum class State_address : uint32_t {
        audio_loop = 0,
        num_states          // sentinel
    };

    static auto make_specs() -> std::vector<tiny::State_spec>
    {
        using enum State_address;
        return {
            tiny::State_spec{ .address = tiny::enum_raw(audio_loop),
                              .string_id = "audio_loop",
                              .name = "Audio Loop" }
        };
    }
};
static_assert(tiny::Some_state_model<State_model>);
```

Default template ships with `num_states` as the only enum value and an empty `make_specs()` (zero items → no overhead).

### Processor additions (optional)

```cpp
// Only required when State_model has num_states > 0
auto save_state_item(uint32_t address) -> std::vector<uint8_t>;
auto load_state_item(uint32_t address, std::span<const uint8_t> data) -> void;
```

A `static_assert` in each format wrapper enforces this: if `State_infos<State_model>::num_states > 0`, then `Has_state_items<Plug_processor>` must be satisfied.

---

## New / Modified Shared Files

### 1. `shared/tinyplug/tiny_state.h` (new)

```cpp
struct State_spec {
    uint32_t address{};
    std::string string_id{};
    std::string name{};
};

template<typename T>
concept Some_state_model = requires {
    typename T::State_address;
    requires Enum<typename T::State_address>;
    requires std::same_as<std::underlying_type_t<typename T::State_address>, uint32_t>;
    { T::make_specs() } -> std::same_as<std::vector<State_spec>>;
};

template<Some_state_model User_model>
class State_infos {
public:
    static constexpr auto num_states =
        enum_raw(User_model::State_address::num_states);
    static auto state_specs() -> const std::vector<State_spec>&;
    static auto state_spec(uint32_t address) -> const State_spec&;
private:
    inline static const std::vector<State_spec> specs = User_model::make_specs();
};

// Concept: processor can supply and receive state item blobs
template<typename T>
concept Has_state_items = requires(T t, uint32_t addr,
                                   std::span<const uint8_t> data) {
    { t.save_state_item(addr) } -> std::same_as<std::vector<uint8_t>>;
    { t.load_state_item(addr, data) } -> std::same_as<void>;
};
```

### 2. `shared/tinyplug/tiny_events.h` (modify)

Add to `Render_event`:

```cpp
struct Load_state_item {
    uint32_t address{};
    std::shared_ptr<const std::vector<uint8_t>> data{};
};
using Render_event = std::variant<Set_param, Ramp_param, Accepted_latency,
                                  Load_state_item>;
```

### 3. `shared/tinyplug/tinyplug.h` (modify)

Add `#include "tiny_state.h"`.

### 4. `shared/tinyplug/state_rules.hpp` (modify)

Add per-format keys/constants:

```cpp
// Dictionary-based formats — add to existing structs
struct Aax  { /* existing */ static constexpr const char state_items[] = "tinyplug-state-items"; };
struct Auv2 { /* existing */ static constexpr const char state_items[] = "tinyplug-state-items"; };
struct Auv3 { /* existing */ static constexpr const char state_items[] = "tinyplug-state-items"; };

// Binary formats — magic sentinel written before the state-item section
static constexpr auto state_items_magic = uint32_t{'sttm'};
```

---

## Wire Format for State Items

### Dictionary formats (AAX, AUv2, AUv3)

One CFData / NSData entry per item, stored under key `"tinyplug-state-<string_id>"`.  
A count key `"tinyplug-num-state-items"` is written for validation on load.  
Missing key on load → that item's `load_state_item` is not called (item stays at its default).

### Binary formats (CLAP, VST3)

State item section is **appended after all existing data** (after editor-state items in CLAP; after parameters in VST3 processor stream). No change to the existing header — backward compatibility is free.

Section layout:
```
uint32_t  state_items_magic  ('sttm')
uint32_t  num_items
for each item:
    uint32_t  key_length
    char[]    string_id        (key_length bytes, no null terminator)
    uint64_t  data_length
    uint8_t[] data             (data_length bytes)
```

**Loading old state (no section):** After reading current data, attempt to read `state_items_magic`:
- CLAP: `clap_istream->read()` returns 0 bytes → EOF → no items.
- VST3: `IBStreamer::readInt32u()` returns `false` → no items.
- If 4 bytes are read but don't match the magic → log warning, skip (treated as no items).

---

## Format Wrapper Changes

All five wrappers include `models/state_model.h` and use:

```cpp
using User_states = State_infos<State_model>;
static constexpr auto num_states = User_states::num_states;
// Guard at compile time:
static_assert(num_states == 0 || Has_state_items<Plug_processor>,
    "State_model has items but Plug_processor does not implement save/load_state_item.");
```

Changes are gated with `if constexpr (num_states > 0)` so zero-item plugins pay no runtime cost.

### `formats/aax/source/aax_parameters.cpp`

- `_build_chunk()` (~L663): after editor state, add `num_state_items` int and one entry per item.
- `SetChunk()` (~L280): after editor state restore, look for `State_rules::Aax::state_items` key; for each entry push `Load_state_item` event to kernel queue.

### `formats/auv2/source/auv2_effect.cpp`

- `SaveState()` (~L496): after editor state CFData, write one CFData per item keyed by `"tinyplug-state-<string_id>"`.
- `RestoreState()` (~L576): after editor state, iterate state specs; if key exists, read CFData and push `Load_state_item`.

### `formats/auv3/source/extension/auv3_AUAudioUnit.mm`

- `fullState` getter (~L690): after editor state NSData, add one NSData entry per item.
- `setFullState:` (~L710): after editor state, iterate state specs; if key exists, push `Load_state_item`.

### `formats/clap/source/clap_plugin.cpp`

- `stateSave()` (~L266): after writing editor state items, write the state item section if `num_states > 0`.
- `stateLoad()` (~L453): after reading editor state, attempt to read state item section; handle read returning 0 as EOF.

### `formats/vst3/source/vst3_processor.cpp`

- `getState()` (~L469): after writing param floats, write state item section if `num_states > 0`.
- `setState()` (~L389): after reading param floats, attempt to read state item section.

---

## Backward Compatibility Assessment

| Scenario | Behaviour |
|---|---|
| Plugin without `State_model` (all existing plugins) | `num_states == 0` → `if constexpr` branches compile away entirely. No change to serialized format. |
| Plugin with `State_model`, loading **old** saved state | State item section absent → no `Load_state_item` events sent → processor keeps defaults. ✅ |
| Plugin with `State_model`, loading **new** saved state | Normal path. ✅ |
| Old plugin binary loading **new** state (forward compat) | Old binary has no state-model code; the appended section / extra dict keys are silently ignored by the format. ✅ |

Existing shipped plugins with no `State_model`: **zero changes required, zero format changes**.

---

## Thread Safety

- **Save** (`save_state_item`): called on the host's serialization thread (main thread in practice). Same guarantee the host provides for `getState` — rendering is expected to be suspended. Plugin author's responsibility to make their blob accessible from that thread (e.g. `atomic<shared_ptr<const vector<uint8_t>>>`).
- **Load** (`load_state_item`): delivered via `Load_state_item` on the `Render_event` queue (same mechanism as `Set_param`), so the processor receives it safely between `process()` calls.

---

## Critical Files

| File | Action |
|---|---|
| `shared/tinyplug/tiny_state.h` | **Create** |
| `shared/tinyplug/tiny_events.h` | Modify — add `Load_state_item` variant |
| `shared/tinyplug/tinyplug.h` | Modify — include `tiny_state.h` |
| `shared/tinyplug/state_rules.hpp` | Modify — add keys and magic constant |
| `template/source/models/state_model.h` | **Create** (default empty model) |
| `template/source/plug_processor.h` | Modify — show optional state methods |
| `formats/aax/source/aax_parameters.cpp` | Modify — save/load state items in chunk |
| `formats/auv2/source/auv2_effect.cpp` | Modify — save/load state items in property list |
| `formats/auv3/source/extension/auv3_AUAudioUnit.mm` | Modify — save/load state items in dictionary |
| `formats/clap/source/clap_plugin.cpp` | Modify — append/read state item section |
| `formats/vst3/source/vst3_processor.cpp` | Modify — append/read state item section |

---

## Verification

1. **Existing plugin builds clean** — compile the template plugin (no `State_model` items); confirm no binary size or behavior change.
2. **Round-trip test** — implement a test plugin with one state item (e.g. a `std::string` serialized to bytes). Save project in each format; reload; confirm item restored.
3. **Old-state compatibility** — save a project with the current (pre-feature) build; load it with the new build; confirm parameters load correctly and no crash/corruption.
4. **Empty item resilience** — load new state into a plugin where `load_state_item` receives 0 bytes for an item (item was missing in the file); confirm graceful no-op.
5. **auval / pluginval / CLAP validator** — run format validators on a plugin using `State_model` to confirm the state round-trip passes host conformance checks.
