# Plan: Notes in, notes out

> Status: **built**, steps 1–7 except the extraction in step 0, which was skipped: the five
> loops differ enough (CLAP and AUv3 walk the host's own event lists, AAX has none) that each
> gained notes in place instead. Every format carries notes in and out; `sine_synth` and
> `step_sequencer` are the demos. Checked with clap-validator, the VST3 validator, the AAX
> validator (same results as `gain_demo`), and scratch CLAP and AUv2 hosts that play notes
> and read note output. Not yet run in a DAW. "Later" below is still later.

## Principles

- **No new model.** What a plug-in carries (audio, notes) is a CMake declaration. The types
  are framework vocabulary, like `process::Event`.
- **Authors never see MIDI.** They see notes and a closed set of performance controls, in
  normalized values, as VST3 does. Wrappers translate through one shared, tested codec.
- **Performance data, not state.** Notes and controls are audio-thread events. They never
  write parameters, are never persisted, never enter undo. Parameters stay the host's.
- **Pipes, not policy.** The framework moves events safely between threads and processes, and
  takes on state only where the state erases a format difference. Note identity qualifies
  (see "Note identity"); voices and an editor keyboard's held keys don't, and stay with the
  plug-in.
- **Sample accurate, like parameters.** In through `handle` between process slices at its
  offset; out with an offset.

## Declaration (CMake)

What the plug-in carries, and the format identity derived from it:

```cmake
add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_AUDIO "out;sidechain")  # in, out, sidechain; default "in;out"
add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_NOTES "in")             # in, out; default none
```

`sidechain` folds in today's `TINY_PLUGIN_WANTS_SIDECHAIN` (kept as a deprecated alias). The
split between a main input and a sidechain is what resolves the one real ambiguity: a
vocoder with a *main* input is an effect that takes notes; with a *sidechain* it is an
instrument.

| Audio | Notes | Derived kind | AUv2 / AUv3 | VST3 | CLAP | AAX |
|---|---|---|---|---|---|---|
| in, out | — | effect | `aufx` | `Fx` | `audio-effect` | as today |
| in, out | in (± out) | effect | `aumf` | `Fx` | `audio-effect` + note port | as today + MIDI node |
| out (± sidechain) | in (± out) | instrument | `aumu` | `Instrument` | `instrument` | `SWGenerators` |
| — | in, out | note effect | `aumi` | `Fx\|Event` | `note-effect` | `MIDIEffect`, audio passed through |
| in, out | out | effect (audio → notes) | `aufx` + MIDI out | `Fx` | `audio-effect` + note port | + MIDI node |

- Anything else (no output of either kind, notes in with nothing out, audio out with no
  input of either kind) is a configure error naming the table. A generator (`augn`) can come
  later as one more row.
- The author's `TINY_AUV2_TYPE` / `TINY_VST3_SUBCATEGORIES` / `TINY_CLAP_FEATURES` /
  `TINY_AAX_CATEGORIES` still add descriptors (`Synth`, `sequencer`). An AUv2 type that
  contradicts the row is an error; the others are appended to the derived token.
- Generated: `Plug_info::kind` and `Plug_info::Wants::{audio_in, audio_out, sidechain,
  notes_in, notes_out}`; in `<tiny_models.hpp>`, `TINY_HAS_NOTES_IN` / `TINY_HAS_NOTES_OUT`
  and `has_notes_in` / `has_notes_out`, so the header-only layer gates as it does for
  meters.

## Types

In `tiny::midi` (re-exported into `tiny::process`), model-free, so they live in core
(`tiny_core/tiny_midi.hpp`) beside the codec. Output also takes `midi::Raw`: any channel voice
message, sent as written. Raw is output only (the case against CC is about input), and SysEx
is still later.

```cpp
struct Note {
    // Which note. `id` is assigned by the framework at `On` and carried by every event for that
    // note, whatever the format supplied: match on it alone. `channel` and `key` are
    // information (pitch), not identity.
    struct Id {
        uint32_t id{};
        uint8_t channel{};
        uint8_t key{};
    };

    struct On { Id note{}; float velocity{}; };   // 0…1
    struct Off { Id note{}; float velocity{}; };  // release velocity, 0…1
    struct Choke { Id note{}; };                  // stop now, no release

    // Per-note controls: VST3 and CLAP natively, poly pressure everywhere, MPE and MIDI 2.0
    // later through the codec.
    struct Expression {
        enum class Kind : uint8_t { Volume, Pan, Tuning, Vibrato, Brightness, Pressure };
        Id note{};
        Kind kind{};
        double value{}; // Tuning in semitones; the rest 0…1.
    };

    using Any = std::variant<On, Off, Choke, Expression>;
};

// Whole-channel performance controls: a closed set, not arbitrary CC (see "Controls").
struct Control {
    struct Bend { uint8_t channel{}; double value{}; };      // −1…1
    struct Pressure { uint8_t channel{}; double value{}; };  // 0…1

    // A controller a player's hardware has: wheels and pedals.
    struct Pedal {
        enum class Kind : uint8_t { Mod_wheel, Breath, Foot, Expression, Sustain, Sostenuto, Soft };
        uint8_t channel{};
        Kind kind{};
        double value{}; // 0…1; switches read >= 0.5.
    };

    using Any = std::variant<Bend, Pressure, Pedal>;
};
```

`Pedal` is a working name for "wheel or pedal" (see N1).

## Processor interface

`Some_plug_processor` becomes `process::Interface`, and its requirements follow the plug-in's
declared capabilities instead of detecting optional members:

```cpp
template<typename T>
concept Interface = requires(T t) {
    { t.configure(std::declval<const Config&>()) } -> std::same_as<void>;
    { t.reset(std::declval<const Reset::Any&>()) } -> std::same_as<void>;
    { t.handle(std::declval<const Event::Any&>()) } -> std::same_as<void>;
    { t.process(std::declval<Dsp_context&>()) } -> std::same_as<void>;
    { t.latency_samps() } -> std::same_as<uint32_t>;
    { t.tail_samps() } -> std::same_as<uint32_t>;
}
#if TINY_HAS_NOTES_IN
&& requires(T t) {
    { t.handle(std::declval<const Note::Any&>()) } -> std::same_as<void>;
    { t.handle(std::declval<const Control::Any&>()) } -> std::same_as<void>;
}
#endif
;
```

- Declaring notes in means handling both; an author who ignores pedals writes an empty
  visitor. Required, not detected: a misspelled overload is a compile error, not silence.
- The generated `<tiny_plugin.hpp>` assertion and every example's
  `static_assert(Some_plug_processor<Processor>)` move to `Interface`, with one `static_assert`
  per capability so the message names the missing overload.
- The same pass applies to the worker reply: `handle_worker_reply(const To_processor&)` is
  required under `TINY_HAS_WORKER` rather than concept-detected (the editor side likewise),
  which retires the detect-in-a-template workaround CLAUDE.md warns about. Worth a separate
  commit inside this work.
- Output through the context, under `TINY_HAS_NOTES_OUT`:

  ```cpp
  context.notes.send(frame, Note::On{{.id = _next_id++, .channel = 0, .key = 60}, 0.8f}); // Output ids are the processor's.
  context.notes.send(frame, Control::Pedal{0, Control::Pedal::Kind::Mod_wheel, 0.5});
  ```

  `frame` is relative to this slice; the writer adds the slice's start. Fixed capacity,
  owned by the wrapper, no allocation; `send` returns false when full.
- Instruments get empty `ibuffers` (or the sidechain only). `Reset::Hard` gains one sentence:
  it also means release every voice and return controls to their defaults.

## Note identity

Hosts own note identity where they have it: VST3's `noteId` and CLAP's `note_id` let a later
`Off` or expression address one exact note. But AU and AAX never carry ids, and VST3 and CLAP
hosts may send -1, so without help every instrument needs two matching rules (by id if there
is one, else by channel + key), which is where cross-format stuck notes come from. That is a
format difference, and erasing those is the framework's job.

So the scheduler keeps a fixed table of held notes (a few hundred entries, no allocation). At
`On` it mints a fresh id and records how the source will name the note again: the host id,
or channel + key. Later `Off`, `Choke` and `Expression` events arrive carrying the minted id;
one for a note the table doesn't know is dropped.

- **One policy:** a second `On` for a held key from an id-less source is a new note, with its
  own id; an id-less `Off` closes the oldest held note on that key, as hardware does. Whether
  the second `On` retriggers or layers is still the plug-in's decision: it sees two ids.
- **Sources are separate, and each may name its own notes.** Every source's ids are local to
  it: the host's `noteId` / `note_id`, or ids the editor chooses. The editor pipe feeds the
  same table under its own source, so an editor `Off` can never close a host note on the same
  key. A source that gives no id is matched on channel + key.
- **Voices stay the plug-in's**: allocation, stealing, polyphony, mono and legato, choke
  groups, release tails. A voice allocator encodes musical policy; if one is ever wanted, it
  is an opt-in helper in `tiny_dsp`, beside `Host_bypass`.
- **Out of scope:** CLAP's `NOTE_END` and voice-info. They exist so the host knows when to stop
  sending per-note parameter modulation, which tinyplug doesn't support. If it ever does,
  the table maps minted ids back to host ids and the processor gains a way to report a
  voice's end.
- Output ids are the processor's: a sequencer names its own notes, and the wrapper maps them
  to the host's note id where the format has one.

## Delivery

- **Ordering.** Sort by `(offset, arrival)`: same-offset order matters (an `Off` then an `On`
  for a retriggered key), and `std::stable_sort` may allocate.
- **Capacity.** A note reserve beside the parameter reserve in every `_events`. On overflow,
  drop `On`s first; never drop an `Off`.
- **Slicing.** Dense notes mean small slices, as dense automation does. Accepted: the
  alternative, a span of notes per block, makes every author split blocks.
- **Five loops.** All five wrappers carry their own collect/sort/slice loop and all five
  change. Extract one shared scheduler in `tinyplug` first (step 0).

## Stuck notes

Stateless. On `Reset::Hard`, bypass engage and deactivate (at the next block, where a format
gives no chance before), the wrapper sends *all notes off* on every channel of the note
output: a wildcard `Off` in CLAP (`key = -1`), CC 123 elsewhere. No held-note tracking.
Everything else, including releasing notes when the transport stops, is the processor's.

**Bypass by kind:** an instrument keeps receiving notes while bypassed, so its voices track,
and its output is silenced. A note effect passes input notes through. Effects are unchanged.

## Controls, and why not CC

**What goes wrong with CC.** CC is trouble when it moves a parameter behind the host's back:
two writers for one value, one of them unrecorded. VST3 dropped raw MIDI for that reason and
routes controllers through `IMidiMapping` into parameters.

**What a plug-in realistically does with CC:**

1. **Performance controllers**: mod wheel, breath, expression, foot, and above all sustain.
   A keyboard instrument without sustain is broken for players. Also pitch bend and
   channel pressure, which VST3 routes the same way.
2. **MPE**: per-channel bend, pressure and CC 74. Hosts turn these into note expressions in
   VST3 and CLAP; the codec does it for AU and AAX. None of it reaches the author as CC.
3. **MIDI learn / CC → parameter.** A plug-in can't write parameters, so all it could do is
   keep a mapping in its state document and apply the CC as a macro or offset on top of
   the parameter's value. That's coherent, but it's worse than what every host already
   offers (controller → automation mapping), and the knob wouldn't move. Agreed with VST3:
   expose parameters and let the host map controllers to them.
4. **Hardware emulation** (a synth that answers its original's CC chart) is (3) again, and
   the host's mapping covers it.

**So:** no arbitrary CC, in or out. A closed set, `Control::{Bend, Pressure, Pedal}`, covers
(1); (2) arrives as `Note::Expression`; (3) and (4) are the host's job, documented as the
reason. Other controllers are dropped by the wrapper in every format, so a plug-in behaves
the same everywhere. The closed set also keeps the author out of MIDI numbers: `Sustain`,
not 64.

**What it costs in VST3:** `IMidiMapping` for 9 controllers (7 pedals, bend, pressure) on 16
channels, 144 hidden, non-automatable, never-persisted parameters in a reserved id range
(like meters' `export_param_offset`), turned back into `Control` events in `process`. Not
JUCE's 2080. Mapping omni instead would cut it to 9 at the price of the channel number; the
channel only matters to a multitimbral instrument, so omni is the fallback if hosts list the
144 (N4). Output uses `LegacyMIDICCOutEvent`.

## Editor → processor (an on-screen keyboard)

A pipe:

```cpp
_edit.notes.send(Note::On{{.channel = 0, .key = 60}, 0.8f});   // -> bool
_edit.notes.send(Note::Off{{.channel = 0, .key = 60}, 0.f});
_edit.notes.send(Control::Bend{0, value});
```

The framework moves `Note::Any` and `Control::Any` from the editor to the processor: in
order, bounded, delivered at the start of the next block (offset 0) through the same
`handle`. `send` returns false when full. That is all it does.

**Weighed against owning it** (`press(key)` / `release(key)`, with the framework tracking
held keys and releasing them on hide): that version saves each keyboard perhaps fifteen
lines and costs a held-key table in the editor, a policy about what "hide" means, and an API
that grows with each gesture a keyboard might want. None of it is needed for coherence: the
hazard it would guard against, an editor `Off` closing a host voice on the same key, is
already closed by note identity, because the editor is its own source. The editor names
notes by channel + key, like an id-less host.

An editor may set `Note::Id::id` to an id of its own (a touch counter, say), mapped like a
host's; without one its notes match on channel + key. It needs one for two fingers on one
key, where channel + key can't say which note a `Note::Expression` means. That is what makes
an in-plug-in keyboard with per-finger pressure or pitch slides work (`Expression` with
`Pressure` or `Tuning`, one per finger per frame, coalesced by the editor).

What the plug-in owns, and the docs and the `sine_synth` keyboard show:

- pairing each `Off` with its `On`;
- releasing held keys in `on_gui_hide`;
- retrying a refused `Off` next frame (a keyboard can't fill the queue at human rates, but a
  host that stops processing can leave it full).

Per format: coupled formats push straight into a processor-side `Lock_free_queue`. VST3
sends a `tiny/notes` `IMessage` and the processor queues it. AAX carries it as a new
`Ring_kind` on the existing inbound `Byte_ring`, inheriting Direct Data's ~30 ms wakeup
(stated as a limit). Not a `Change_set`: order matters.

No format lets a plug-in inject notes into its own track, so an on-screen keyboard plays the
plug-in, not the DAW. Showing held notes in the editor needs nothing new: the processor
publishes a held-key bitmap as a block.

## Formats

One codec in core (`tiny_core/midi_codec.hpp`: MIDI 1.0 bytes ↔ `Note` / `Control`, 14-bit
bend, the closed controller table) serves AUv2, AUv3, AAX and CLAP's MIDI dialect. Tested
once, standalone.

| | Input | Output | Notes |
|---|---|---|---|
| **CLAP** | `CLAP_EVENT_NOTE_*`, `NOTE_EXPRESSION`, `MIDI` (codec) | `out_events`, CLAP dialect | `note_ports`; dialects `CLAP \| MIDI`, prefer `CLAP` |
| **VST3** | `inputEvents` (`NoteOn/Off`, `PolyPressure`, `NoteExpressionValue`); controls via `IMidiMapping` | `outputEvents`; controls via `LegacyMIDICCOutEvent` | event buses in `initialize`; bus arrangement accepts no audio input |
| **AUv2** | `AUMIDIBase` mixed into the existing class when notes are in; factory chosen by kind | `kAudioUnitProperty_MIDIOutputCallback`, after render | MIDI entry points can arrive off the render thread: they use the timed queue `_to_processor` already provides |
| **AUv3** | `AURenderEventMIDI` in the render event list | `MIDIOutputEventBlock` + `MIDIOutputNames` | component type from the kind, in the plist |
| **AAX** | `AAX_IMIDINode` (`LocalInput`) as an `Alg_context` field; packets carry sample timestamps | `LocalOutput` node | reaches the algorithm directly; instrument input stem `None` |

## MPE and MIDI 2.0

Both fit behind the same types; neither changes the author's API.

- **MPE.** VST3 and CLAP hosts already deliver it as `Note::Expression` (CLAP when the
  plug-in accepts the `MIDI_MPE` dialect). For AU and AAX the codec gains MPE zone state
  (the MCM RPN) and turns member-channel bend, pressure and CC 74 into
  `Note::Expression{Tuning, Pressure, Brightness}`, manager-channel messages into `Control`.
  Opt-in, because it changes how channel messages are read: `TINY_PLUGIN_WANTS_NOTES
  "in;expression"`, which also declares CLAP's MPE dialect and VST3's expression types.
- **MIDI 2.0.** AUv3's `AURenderEventMIDIEventList` and CLAP's `MIDI2` dialect carry UMP; the
  codec decodes it. Per-note controllers become `Note::Expression`, 16-bit velocity and
  32-bit controllers fit the float and double fields, and note ids arrive natively. New
  fields MIDI 2.0 adds (attribute types, per-note pitch) extend `On` without breaking anyone
  who initializes by name.
- **Out** is harder than in: sending MPE means allocating member channels in the encoder.
  Later again.

## Not included

The shape: **abstract types in, abstract types plus raw MIDI 1.0 out.** Input is interpreted
(named notes, a closed set of controls) because the framework has to keep it coherent: note
identity across formats, no second writer for parameters. Output needs no such guarding, so
it can be as raw as a device downstream wants. What neither side carries yet:

**In**

- Arbitrary CC, NRPN / RPN, 14-bit CC pairs: dropped. Player controls are the closed
  `Control` set; everything else is the host's job, mapped to parameters (MIDI learn is out
  on purpose).
- Program change and bank select: dropped. Hosts load presets.
- SysEx, and system messages (clock, song position, MTC, start / stop): dropped. Tempo and
  transport arrive as `Musical_context`.
- Channel mode messages, except all-notes-off and all-sound-off, which release the held
  notes they name.
- MPE on AU and AAX (VST3 and CLAP hosts already deliver it as `Note::Expression`), and MIDI
  2.0 / UMP anywhere. Both are planned behind the same types (see above).
- VST3 note-expression declarations, note names and key switches (CLAP `note_name`, VST3
  `INoteExpressionController` / `IKeyswitchController`), CLAP `NOTE_END` and voice info, and
  polyphonic parameter modulation.

**Out**

- SysEx. It needs a fixed byte pool in the outbox to stay allocation-free.
- System messages: clock, song position, start / stop. VST3 can't express them.
- MIDI 2.0, and MPE out (allocating member channels).
- Note ids reaching the host where the format has none: AU, AAX, and `midi::Raw` notes in
  VST3 go out on channel + key.

**Around it**

- An on-screen keyboard plays the plug-in; no format lets a plug-in record into its own track.
- Per-format identity (an `aumi` for Logic and an instrument for VST3 from one declaration).
  Today a plug-in picks one shape for every format.
- Where a host puts a notes-only plug-in is the host's policy: Live, for one, loads neither
  `aumi` nor an audio-less VST3 effect (see N5).

## Demos

- **`sine_synth`** (audio out; notes in): eight voices, ADSR and level parameters, voice
  matching on `Note::Id`, pitch bend, sustain. An on-screen keyboard through `_edit.notes`,
  owning its ids and held keys, highlighting held keys from a block.
- **`step_sequencer`** (notes in and out, no audio): a 16-step pattern in a `Writers::Editor`
  state document, so it saves, undoes and edits from the UI only. The processor reads the
  pattern and `Musical_context`, emits `On`/`Off` at step boundaries while the transport
  moves, releases its own notes on stop, and passes input notes through. Built for CLAP,
  VST3, AUv2 (Logic MIDI FX), AUv3 and AAX (a MIDI effect on Instrument tracks).

## Order of work

0. Extract the shared event scheduler from the five wrappers (behavior-preserving). The
   note-identity table lands in it with step 4.
1. `process::Interface` with capability-gated requirements; examples and template follow.
   The worker reply joins it in its own commit.
2. Types and codec in core, with a standalone codec test.
3. CMake `WANTS_AUDIO` / `WANTS_NOTES`, derivation and validation, `Plug_info`,
   `<tiny_models.hpp>` gating, format identity and bus layouts. Existing demos build
   unchanged.
4. Input: CLAP, VST3 (with `IMidiMapping`), AUv3, AUv2, AAX. `sine_synth` without its
   keyboard.
5. Output and all-notes-off: CLAP, VST3, AUv2, AUv3 (AAX's output node built, unused by the
   demos). `step_sequencer`.
6. The editor pipe and its per-format transport; the `sine_synth` keyboard.
7. Validators (clap-validator, the VST3 validator, auval, the AAX validator) and a pass in
   Logic, Reaper, Bitwig and Pro Tools.

## Later

- Touch force and radius in the platform's pointer events (`UITouch.force`, `majorRadius`),
  so an iOS keyboard can drive per-note pressure. iPad fingers report no force; radius is the
  usual stand-in.
- MPE and MIDI 2.0, as above.
- A generator row (`augn`).
- Note names and key switches (CLAP `note_name`, VST3 `INoteExpressionController` /
  `IKeyswitchController`); VST3 note expression declarations.

## Open questions

- **N1. Names.** `Note` / `Control` / `Control::Pedal`, `context.notes`, `_edit.notes`.
  `context.notes.send` carries controls too; `context.output` is the alternative.
- **N2. Note-effect audio.** Logic's `aumi` and some VST3 hosts want an audio output bus even
  on a MIDI effect. A silent bus, or none? Settle per format in step 3.
- **N3. AAX note effects.** Closed, and the premise was wrong: AAX has
  `AAX_ePlugInCategory_MIDIEffect`, which Pro Tools lists in a separate MIDI plug-ins menu on
  Instrument tracks and chains through the first channel of the first MIDI input and output
  node. A MIDI effect is still an audio insert, so it passes audio through and registers every
  stem format it can (mono and stereo here); the processor never sees the audio.
- **N5. Where hosts put a notes-only plug-in.** Every spec allows the combination except
  VST3's, which has no category for it (Steinberg has said there are no plans for one). Live
  refuses an `Fx` VST3 without an audio input and doesn't scan `aumi`; the known way into Live
  is an instrument with notes out (`TINY_PLUGIN_WANTS_AUDIO "out"`), routed with "MIDI From".
  JUCE's MIDI-effect builds add a dummy audio output for the same reason. Per-format identity
  (`aumi` for Logic, an instrument in VST3) is a possible later opt-in.
- **N4. VST3 controls.** 144 hidden parameters per channel-aware mapping, or 9 mapped omni.
  Start channel-aware; fall back if hosts list them.
