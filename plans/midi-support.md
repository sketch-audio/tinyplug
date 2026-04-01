# Plan: Add MIDI Input and Output Support to tinyplug

## Context

tinyplug is currently effects-only. This plan adds MIDI input/output support to enable **instrument plugins** (synths, samplers) and **MIDI effect plugins** (arpeggiators, transposers) alongside existing audio effects. The framework's core strength is that users write format-agnostic code — a single `Plug_processor` and `Plug_editor` — while format wrappers handle VST3/CLAP/AUv2/AUv3/AAX translation. MIDI support must preserve this property.

The design priority is the **framework abstraction quality** — a clean, minimal user-facing API that hides format differences. Format wrapper implementation details are outlined but secondary.

---

## 1. MIDI Event Types (`shared/tinyplug/tiny_events.h`)

Add structured note events rather than raw MIDI bytes. This matches the direction of modern formats (VST3, CLAP) and is a better abstraction. Format wrappers translate raw MIDI (AU, AAX) into these types.

### New types

```cpp
// Unified note addressing across all formats.
// VST3/CLAP provide explicit voice IDs; AU/AAX use channel+key only.
// -1 means "unspecified" / wildcard.
struct Note_id {
    int16_t channel{};   // 0-15, or -1
    int16_t key{};       // 0-127 MIDI note number, or -1
    int32_t id{-1};      // Unique voice ID (VST3/CLAP only, -1 for AU/AAX)
};

struct Note_on {
    Note_id note{};
    double velocity{};   // 0.0 - 1.0
};

struct Note_off {
    Note_id note{};
    double velocity{};   // 0.0 - 1.0 (release velocity)
};

// Immediately kill a voice without release phase (CLAP note_choke).
struct Note_choke {
    Note_id note{};
};

// Per-note expression (polyphonic modulation).
// Only natively supported in VST3/CLAP; ignored on AU/AAX.
enum class Note_expression : uint32_t {
    volume = 0, pan, tuning, vibrato, expression, brightness, pressure,
};

struct Note_expression_value {
    Note_id note{};
    Note_expression expression{};
    double value{};
};

// Channel-level MIDI messages.
struct Midi_cc {
    int16_t channel{};
    uint8_t cc{};        // 0-127
    double value{};      // 0.0 - 1.0
};

struct Pitch_bend {
    int16_t channel{};
    double value{};      // -1.0 to 1.0
};

struct Channel_pressure {
    int16_t channel{};
    double value{};      // 0.0 - 1.0
};
```

### Updated Render_event variant

```cpp
using Render_event = std::variant<
    Set_param, Ramp_param, Accepted_latency,
    Note_on, Note_off, Note_choke,
    Note_expression_value,
    Midi_cc, Pitch_bend, Channel_pressure
>;
```

`Tagged_event` is unchanged — it already wraps `Render_event` with a sample-accurate offset.

### MIDI output types (separate variant)

```cpp
using Midi_output_event = std::variant<
    Note_on, Note_off, Note_choke, Midi_cc, Pitch_bend, Channel_pressure
>;

struct Tagged_midi_output {
    Midi_output_event event{};
    int32_t offset{};
};
```

### Design rationale
- **Normalized doubles** (0.0–1.0 for velocity/CC) instead of raw MIDI integers — format wrappers convert.
- **`Note_id`** unifies VST3 `noteId`, CLAP `note_id`, and channel+key for AU/AAX. Formats without voice IDs leave `id` at -1.
- **`Note_choke`** is distinct from `Note_off` — it means "kill immediately, no release phase" (important for drum machines).
- **Backward compatible** — existing effect plugins that only handle `Set_param`/`Ramp_param`/`Accepted_latency` in their `handle_event` visitor continue working unchanged. New variant alternatives fall through to the default `[](const auto&) {}` handler.

---

## 2. Processor Interface Changes (`shared/tinyplug/tiny_processor.h`)

### Dsp_context additions

```cpp
struct Dsp_context {
    // ... existing fields unchanged ...

    // MIDI output buffer. Non-null only for plugins with wants_midi_output.
    // Processor appends events here during process().
    // Framework reads and dispatches to host after process() returns.
    std::vector<Tagged_midi_output>* midi_output{nullptr};
};
```

### Processor concept — UNCHANGED

```cpp
concept Some_plug_processor = requires(T t) {
    { t.reset(double{}) } -> std::same_as<void>;
    { t.handle_event(std::declval<const Render_event&>()) } -> std::same_as<void>;
    { t.process(std::declval<Dsp_context&>()) } -> std::same_as<void>;
    { t.latency_samps() } -> std::same_as<uint32_t>;
    { t.tail_samps() } -> std::same_as<uint32_t>;
};
```

This is a key advantage of the existing variant-based design: MIDI events are new alternatives in `Render_event`, so `handle_event` receives them automatically. Zero concept changes needed.

---

## 3. Sample-Accurate Event Interleaving — NO ARCHITECTURAL CHANGE

Every format wrapper already implements the same pattern:
1. Collect all events into `_events` vector with sample offsets
2. Sort by offset
3. Walk the buffer: process audio up to next event → dispatch event → continue

MIDI events slot directly into this mechanism. Each wrapper's event-collection phase now *also* collects MIDI input events, converts them to `Tagged_event` with the appropriate `Render_event` alternative, and pushes them into `_events`. The existing sort-and-dispatch loop handles everything.

---

## 4. Plugin Type Configuration

### New CMake property

```cmake
add_property(${target} TINY_PLUGIN_TYPE "effect")  # "effect" | "instrument" | "midi_effect"
```

Default is `"effect"` for backward compatibility.

### Changes to `cmake/helpers.cmake`

Add a derivation function that maps `TINY_PLUGIN_TYPE` → format-specific values:

| `TINY_PLUGIN_TYPE` | AUv2 type | VST3 subcategories | CLAP features | AAX category |
|---|---|---|---|---|
| `"effect"` | `aufx` (existing) | `Fx` (existing) | `audio-effect` (existing) | existing |
| `"instrument"` | `aumu` | `Instrument` | `instrument` (prepended) | `SWGenerators` |
| `"midi_effect"` | `aumi` | `Fx\|Event` | `note-effect` (prepended) | N/A (see below) |

The user writes `add_property(${target} TINY_PLUGIN_TYPE "instrument")` and everything derives. Individual format fields can still be overridden.

### Changes to `cmake/plug_info.h.in`

```cpp
enum class Plugin_type : uint32_t { effect = 0, instrument, midi_effect };

struct Plug_info {
    // ... existing fields ...
    static constexpr auto plugin_type = Plugin_type::@TINY_PLUGIN_TYPE_ENUM@;

    // Derived convenience flags for format wrappers:
    static constexpr auto wants_midi_input = (plugin_type != Plugin_type::effect);
    static constexpr auto wants_midi_output = (plugin_type == Plugin_type::midi_effect);

    // Per-note expression support. When true:
    //   - CLAP: declares CLAP_NOTE_DIALECT_CLAP | MIDI_MPE | MIDI2
    //   - VST3: registers note expression types
    //   - AUv2/AUv3/AAX: enables MPE zone parsing in the wrapper
    // When false: note events are delivered but per-note expression is ignored.
    // Default false — simple synths just get Note_on/Note_off.
    static constexpr auto supports_per_note_expression = bool{@TINY_SUPPORTS_PER_NOTE_EXPRESSION@};
};
```

### Files to modify
- `cmake/plug_info.h.in` — add `Plugin_type` enum and derived booleans
- `cmake/helpers.cmake` — add type derivation in `configure_plug_info`

---

## 5. MIDI Version Handling (MIDI 1.0 / MPE / MIDI 2.0)

The framework's structured event types (`Note_on`, `Note_off`, `Note_expression_value`, etc.) already provide a version-agnostic abstraction. **Plugin developers never see raw MIDI bytes or need to know which protocol delivered the data.** Format wrappers handle all translation.

### How each MIDI version maps to the framework

**MIDI 1.0 (standard)**
- Notes → `Note_on`/`Note_off` with `Note_id{channel, key, -1}` (no voice ID)
- Pitch bend, CC, channel pressure → global `Pitch_bend`/`Midi_cc`/`Channel_pressure` events (affect all notes on that channel)
- 7-bit velocity/CC values normalized to `double` by wrappers
- No per-note expression capability

**MIDI 1.0 + MPE**
- MPE uses channel-per-note allocation within zones. Manager channel sends global controls; member channels each carry one note with per-note pitch bend (tuning), channel pressure (pressure), and CC#74 (brightness)
- Wrappers that receive raw MIDI (AUv2, AUv3, AAX) translate MPE member channel messages into `Note_expression_value` events:
  - Member pitch bend → `Note_expression_value{note, tuning, ...}` (converted from semitone range)
  - Member channel pressure → `Note_expression_value{note, pressure, ...}`
  - Member CC#74 → `Note_expression_value{note, brightness, ...}`
  - Manager channel messages → global `Pitch_bend`/`Midi_cc` events
- VST3/CLAP hosts already parse MPE for us — they deliver structured note events with voice IDs and note expressions. Wrappers just translate to framework types.

**MIDI 2.0**
- Native per-note attributes, 32-bit resolution, note IDs built into the protocol
- Maps directly to framework types — `Note_id.id` carries the note ID, `double` values absorb 32-bit resolution
- Current host support is minimal. CLAP has `CLAP_NOTE_DIALECT_MIDI2`; AUv3 can receive UMP packets on newer macOS. Implementation can wait — the abstraction already supports it.

### No "dialect" declaration needed

The plugin developer declares **capability**, not protocol:

```cpp
// In Plug_info:
static constexpr auto supports_per_note_expression = bool{false}; // default
```

This single boolean controls wrapper behavior across all formats:

| `supports_per_note_expression` | CLAP dialects | VST3 | AUv2/AUv3/AAX |
|---|---|---|---|
| `false` | `CLAP \| MIDI` | No note expression types | No MPE parsing — pitch bend/CC/pressure delivered as global channel events |
| `true` | `CLAP \| MIDI_MPE \| MIDI2` | Registers note expression types | Enables MPE zone parsing — per-channel messages converted to `Note_expression_value` |

CLAP always prefers `CLAP_NOTE_DIALECT_CLAP`. The supported set tells the host what fallbacks are acceptable.

A simple synth sets nothing (defaults to `false`) and gets plain `Note_on`/`Note_off`. An expressive synth (e.g., for Roli Seaboard/Linnstrument) sets `supports_per_note_expression = true` and receives `Note_expression_value` events — regardless of whether the source was MPE, CLAP native, VST3 note expression, or MIDI 2.0.

### MPE Zone Parsing (shared utility)

For raw-MIDI formats (AUv2, AUv3, AAX) when `supports_per_note_expression` is true, the framework provides a shared `Mpe_state` utility in `shared/tinyplug/`:

```cpp
// Tracks MPE zone configuration and active note→channel mappings.
// Used by AUv2/AUv3/AAX wrappers to convert raw MIDI into Note_expression_value events.
struct Mpe_state {
    // Call when receiving RPN 0x0006 (MCM message) to configure zone boundaries.
    void handle_mcm(uint8_t channel, uint8_t member_channel_count);

    // Returns true if the given channel is a member channel in an active zone.
    bool is_member_channel(uint8_t channel) const;

    // Track note-on/off to maintain channel→active note mapping.
    void note_on(uint8_t channel, uint8_t key);
    void note_off(uint8_t channel, uint8_t key);

    // Look up which note is active on a given member channel.
    // Returns the Note_id, or nullopt if no note is active.
    std::optional<Note_id> active_note(uint8_t channel) const;
};
```

This is a lightweight state tracker (no allocations, fixed-size arrays for 16 channels). Each raw-MIDI wrapper creates one and feeds it MIDI events during the event collection phase. When a pitch bend/pressure/CC#74 arrives on a member channel, the wrapper checks `active_note()` and emits the corresponding `Note_expression_value`.

### Resolution handling

All resolution differences are absorbed by the `double` value space:
- MIDI 1.0: 7-bit (0-127) → wrapper divides by 127.0
- MIDI 1.0 pitch bend: 14-bit (0-16383) → wrapper maps to -1.0..1.0
- MIDI 2.0: 32-bit → wrapper divides by max uint32
- VST3/CLAP: already provide float/double values

The plugin developer always sees normalized doubles. Zero protocol awareness required.

---

## 6. Format Wrapper Changes

### 6.1 VST3 (`formats/vst3/source/vst3_processor.cpp`)

**MIDI Input:**
- `initialize()`: Add event input bus (`addEventInput(u"Event In", 16)`) when `Plug_info::wants_midi_input`.
- `normalize_input_events()`: In addition to `IParameterChanges`, iterate `data.inputEvents` (`IEventList`). Translate `NoteOnEvent` → `Note_on`, `NoteOffEvent` → `Note_off`, `PolyPressureEvent` → `Note_expression_value{..., pressure}`, `NoteExpressionValueEvent` → `Note_expression_value`. Push as `Tagged_event` with `event.sampleOffset`.
- Existing sort-and-dispatch loop handles the rest.

**MIDI Output:**
- After process loop: iterate `context.midi_output`, translate to VST3 `Event` structs, push to `data.outputEvents`.

**Bus config:**
- Instruments: main audio input bus is optional. `setBusArrangements` needs a path where 0 audio inputs is acceptable.

**Category:**
- `Plug_info::Vst3::subcategories` already flows into the factory entry. `"Instrument"` is sufficient.

### 6.2 CLAP (`formats/clap/source/clap_plugin.cpp`, `formats/clap/source/clap_plugin.h`)

**MIDI Input:**
- `_handle_host_event()`: Add cases for `CLAP_EVENT_NOTE_ON`, `CLAP_EVENT_NOTE_OFF`, `CLAP_EVENT_NOTE_CHOKE`, `CLAP_EVENT_NOTE_EXPRESSION`, `CLAP_EVENT_MIDI`. Convert to framework types and dispatch to processor.
- Already naturally integrates with the existing event interleaving loop.

**MIDI Output:**
- After process loop: iterate `context.midi_output`, push to `process->out_events->try_push()` as `clap_event_note` structs.

**New extension:**
- Implement `clap_plugin_note_ports`: `notePortsCount()` / `notePortsInfo()`. Instruments: 1 input port. MIDI effects: 1 input + 1 output.
- Dialect negotiation based on `Plug_info::supports_per_note_expression`:
  - `false`: `supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI`
  - `true`: `supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2`
  - `preferred_dialect = CLAP_NOTE_DIALECT_CLAP` always (hosts convert to CLAP native events when possible)

**Descriptor:**
- `Plug_info::Clap::features` already injected into descriptor. `"instrument"` gets prepended by CMake derivation.

### 6.3 AUv2 (`formats/auv2/source/auv2_effect.h`, `formats/auv2/source/auv2_effect.cpp`)

**This is the most significant wrapper change.** Currently `Auv2_effect` inherits from `ausdk::AUBase`. Instruments must inherit from `ausdk::MusicDeviceBase` (which provides `HandleNoteOn/Off`, `HandleControlChange`, etc.).

**Approach:** Create a parallel class `Auv2_instrument` inheriting from `MusicDeviceBase`, sharing common logic via extracted utility functions. CMake selects which source file to compile based on `TINY_PLUGIN_TYPE`. This avoids complex conditional inheritance and keeps each class clean.

**MIDI Input:**
- Override `HandleNoteOn(channel, key, velocity, offsetSampleFrame)` → push `Note_on` into `_events` with offset.
- Override `HandleNoteOff`, `HandleControlChange`, `HandlePitchWheel`, `HandleChannelPressure`.

**MIDI Output:**
- **Not supported on AUv2.** The AU SDK has no standard MIDI output mechanism for AUv2. This is a known limitation to document. `wants_midi_output` is a no-op on AUv2.

**MPE support (when `supports_per_note_expression`):**
- Create `Mpe_state` instance in the wrapper. Feed all MIDI events through it.
- When pitch bend/pressure/CC#74 arrive on an MPE member channel, look up `active_note()` and emit `Note_expression_value` instead of global channel events.
- Zone configuration via RPN 0x0006 (MCM) handled automatically by `Mpe_state`.

**Limitations:**
- AUv2 MIDI handlers do receive `inOffsetSampleFrame` so sample-accurate MIDI is possible.
- No native per-note expressions or voice ID tracking (channel+key only). MPE parsing via `Mpe_state` bridges this gap.

**Component type:**
- `Plug_info::Auv2::type` set to `'aumu'` (instrument) or `'aumi'` (MIDI effect) by CMake derivation.

### 6.4 AUv3 (`formats/auv3/`)

**MIDI Input:**
- In the DSP kernel's event handler: add cases for `AURenderEventMIDI`. Parse raw MIDI bytes from `event->MIDI.data` into framework note types. AUv3 provides sample-accurate timing via `eventSampleTime`.

**MIDI Output:**
- AUv3 supports MIDI output via `AUMIDIOutputEventBlock` (macOS 10.13+ / iOS 11+). Set `self.MIDIOutputNames` and call the output block during render.

**Component type:**
- Set via Info.plist `AudioComponents` array, same mechanism as AUv2.

### 6.5 AAX (`formats/aax/source/aax_parameters.cpp`, `formats/aax/source/aax_describe.cpp`)

**MIDI Input:**
- `RenderAudio()`: Iterate `ioRenderInfo->mInputNode->GetNodeBuffer()` to get `AAX_CMidiStream`. Parse each `AAX_CMidiPacket` (status byte + data bytes + `mTimestamp`) into framework types. Push as `Tagged_event`.
- `aax_describe.cpp`: Set `info.mNeedsInputMIDI = true` for instruments/MIDI effects.

**MIDI Output:**
- AAX supports output via `AAX_IMIDINode::PostMIDIPacket()`. Add output MIDI node in descriptor. After process loop, translate `Tagged_midi_output` to `AAX_CMidiPacket` and post.

**Category:**
- `AAX_ePlugInCategory_SWGenerators` for instruments.

**Bus config:**
- For instruments: `mInputStemFormat = AAX_eStemFormat_None` when no audio input needed.

---

## 7. Format Compatibility Matrix

| Capability | VST3 | CLAP | AUv2 | AUv3 | AAX |
|---|---|---|---|---|---|
| MIDI input (notes) | Yes | Yes | Yes | Yes | Yes |
| Sample-accurate MIDI | Yes | Yes | Yes | Yes | Yes |
| Note ID / voice tracking | Yes (`noteId`) | Yes (`note_id`) | No (ch+key) | No (ch+key) | No (ch+key) |
| Per-note expressions (native) | Yes | Yes | No | No | No |
| MPE → per-note expressions | Host handles | Host handles | Wrapper parses | Wrapper parses | Wrapper parses |
| MIDI 2.0 | Partial* | `MIDI2` dialect | No | UMP (future) | No |
| MIDI CC input | Deprecated** | Yes | Yes | Yes | Yes |
| MIDI output | Yes | Yes | **No** | Yes | Yes |
| Instrument category | `"Instrument"` | `"instrument"` | `'aumu'` | `'aumu'` | enum |

\* VST3 doesn't define a MIDI 2.0 path; some hosts may bridge UMP to VST3 note events.
\** VST3 deprecated raw MIDI CC in favor of parameter automation. Hosts may not send MIDI CC through the event list.

### Key limitations to document
- **AUv2**: No MIDI output. No native per-note expressions (MPE parsing available via `Mpe_state`). No voice ID tracking.
- **AUv3**: No native per-note expressions (MPE parsing available via `Mpe_state`). No voice ID tracking.
- **AAX**: No native per-note expressions (MPE parsing available via `Mpe_state`). No voice ID tracking. No MIDI effect category (AAX doesn't distinguish MIDI effects from instruments).
- **VST3**: MIDI CC delivery is host-dependent due to deprecation. No MIDI 2.0 protocol support.
- **MPE everywhere**: When `supports_per_note_expression` is true, all formats deliver per-note expression — VST3/CLAP via host-native mechanisms, AUv2/AUv3/AAX via framework's `Mpe_state` parser.

---

## 8. Implementation Sequence

### Phase 1: Core types (no format changes, no breaking changes)
1. Add MIDI event types to `shared/tinyplug/tiny_events.h`
2. Add `Plugin_type` enum and `Mpe_state` utility to `shared/tinyplug/`
3. Add `midi_output` pointer to `Dsp_context` in `shared/tinyplug/tiny_processor.h`
4. Verify existing plugins still compile (variant grows but nothing references new alternatives)

### Phase 2: Build system and Plug_info
5. Add `TINY_PLUGIN_TYPE` and `TINY_SUPPORTS_PER_NOTE_EXPRESSION` properties to `cmake/helpers.cmake`
6. Update `cmake/plug_info.h.in` with `Plugin_type`, derived booleans, and `supports_per_note_expression`
7. Update existing plugin CMakeLists to set `TINY_PLUGIN_TYPE "effect"` (explicit default)

### Phase 3: Format wrappers — MIDI input
8. CLAP: note event handling in `_handle_host_event`, note ports extension with dialect negotiation based on `supports_per_note_expression`
9. VST3: event input bus, `inputEvents` reading in `normalize_input_events`, note expression type registration
10. AAX: MIDI node reading in `RenderAudio`, descriptor changes, `Mpe_state` integration
11. AUv3: MIDI event cases in kernel event handler, `Mpe_state` integration
12. AUv2: Create `Auv2_instrument` class with `MusicDeviceBase` inheritance, `Mpe_state` integration

### Phase 4: Format wrappers — MIDI output
13. CLAP, VST3, AUv3, AAX: translate `midi_output` vector to host-native output
14. Document AUv2 limitation

### Phase 5: Instrument bus configuration
15. Each format: conditional audio input bus (optional for instruments)

### Phase 6: Demo and testing
16. Create a simple synth demo plugin (basic Note_on/Note_off)
17. Create an expressive synth demo plugin (with `supports_per_note_expression = true`)
18. Test across formats in DAWs (Logic, Reaper, Bitwig, Pro Tools) with standard MIDI and MPE controllers

---

## 9. Open Design Decisions

1. **AUv2 class split strategy**: Two separate files (`auv2_effect.cpp` / `auv2_instrument.cpp`) sharing utilities via extracted functions, or a CRTP base template? I recommend the simpler split approach — the AU SDK base classes have fundamentally different APIs so trying to unify with templates adds complexity for little gain.

2. **MIDI CC in VST3**: Since VST3 deprecated raw MIDI, should the VST3 wrapper attempt to map host parameter automation back to `Midi_cc` events? Or just document that CC-based workflows should use CLAP/AU/AAX? I'd lean toward "document the limitation" — trying to reverse-map parameters to CC is fragile.

3. **Event buffer sizing**: The `_events` vector capacity should increase for MIDI-capable plugins. Current sizing is `4 * num_params + 64 * bit_width(num_params)`. For MIDI plugins, add a fixed MIDI reservation (e.g., 256 events). Fast arpeggios or dense MIDI can generate many events per buffer.

4. **`midi_effect` plugins with audio**: Should MIDI effects also receive audio input (e.g., a vocoder that takes audio + MIDI)? The `Plugin_type` could be extended, or this could be a combination of flags. For now, `midi_effect` means MIDI-in → MIDI-out with optional audio pass-through.

---

## Verification

- Compile all existing demo plugins (gain_demo, latency_demo, etc.) — they must build unchanged
- Create a minimal synth demo that responds to Note_on/Note_off
- Build as VST3 and CLAP, load in Reaper/Bitwig, verify notes trigger audio
- Build as AUv2, load in Logic, verify notes trigger audio
- Build as AAX (if toolchain available), load in Pro Tools
- Test MIDI output with a MIDI effect demo in CLAP and VST3
