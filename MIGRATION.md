# Migration guide

How to port a downstream plug-in across breaking framework changes. Each section covers one
change, newest last, and says which release it breaks from. Work through them in order.

Every change here breaks the build, so nothing changes behaviour silently. If it builds, it
is almost certainly right.

---

## Model layer

Breaks from `release/0.4`. Splits the framework into a model-free core and a header-only,
model-aware interface, and generates model discovery from CMake. The *why* is in
[plans/model-layer.md](plans/model-layer.md).

### 0. What changed, in one screen

```
BEFORE                                         AFTER
libs/tinyplug (STATIC)                         libs/tiny_core (STATIC, model-free)
                                               libs/tinyplug  (INTERFACE, model-aware)
${TINY_SHARED_LIB}                             ${TINY_PLUG_LIB}  (links ${TINY_CORE_LIB})
<tinyplug/value_helper.hpp> etc.               <tiny_core/value_helper.hpp> etc.
__has_include("worker.hpp")                    configure_models() + configure_plugin()
namespace tiny::plugin { class Editor; }       namespace tiny::edit { class Editor; }
namespace tiny::plugin { class Worker; }       namespace tiny::work { class Worker; }
Worker::Model { From_processor ... }           models::Work in source/models/work.hpp
Worker_reply_actor<Worker>                     Worker_replies
```

Seven changes, in the order to make them:

1. CMake: link `${TINY_PLUG_LIB}` and call the two generators.
2. Include paths: model-free headers moved to `<tiny_core/...>`.
3. Model headers include core only.
4. `tiny::plugin::Editor` → `tiny::edit::Editor`.
5. The worker's channel shape moves out of the class into `models/work.hpp`, and the class
   moves to `tiny::work`.
6. Drop the local `User_params` / `User_meters` aliases (optional, but they are now provided).
7. Meters are optional. Delete an empty `models/meters.hpp`.

### 1. CMake

```cmake
# BEFORE
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_SHARED_LIB})

# AFTER
configure_plug_info(${PLUGIN_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/plug_info.hpp)
configure_models(${PLUGIN_TARGET}) # <tiny_models.hpp> from source/models/
configure_plugin(${PLUGIN_TARGET}) # <tiny_plugin.hpp> from source/
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_PLUG_LIB})
```

`${TINY_SHARED_LIB}` / `tiny_shared_lib` no longer exist. Use `${TINY_PLUG_LIB}` (`tinyplug`)
for a plug-in, or `${TINY_CORE_LIB}` (`tiny_core`) for something that needs no models.

`configure_models` and `configure_plugin` find your headers by **file presence** under
`source/`. Pass `SOURCE_DIR <dir>` if your sources live somewhere else. Each one puts its
generated header on the target's PUBLIC include path.

| File | Required | Absent ⇒ |
|---|---|---|
| `source/models/params.hpp` | yes (for now) | configure error |
| `source/models/meters.hpp` | no | `meters::None`, `TINY_HAS_METERS 0` |
| `source/models/work.hpp` | with `worker.hpp` | `work::None`, `TINY_HAS_WORK 0` |
| `source/processor.hpp` | yes | configure error |
| `source/editor.hpp` | yes (for now) | configure error |
| `source/worker.hpp` | with `models/work.hpp` | `work::None`, `TINY_HAS_WORKER 0` |

Adding or removing one of these files re-runs configure on the next build (the discovery
globs are `CONFIGURE_DEPENDS`), so you don't need to re-run cmake by hand. **Xcode is the
exception:** building a single target with `cmake --build --target X` doesn't run
`ZERO_CHECK`. Build `ZERO_CHECK` (or re-run the preset) after adding or removing a model.

### 2. Include paths

Everything that doesn't depend on your models moved to `libs/tiny_core`. Replace the prefix:

```
tinyplug/{change_list, denormal_guard, gesture_recognizers, host_formatter,
          lifecycle_probe, lock_free_queue, meter_mailbox, meter_publisher,
          notification_queue, platform_defs, relay, serial_queue, state_adapter,
          state_rules, task_launcher, task_manager, tiny_log, tiny_meters,
          tiny_params, tiny_utils, value_helper, window_token}.hpp
   →  tiny_core/<same>.hpp
```

`<tinyplug/tinyplug.hpp>` is still the umbrella for processor, editor and worker code, and
still reaches everything. Two headers were split, and the core halves are new:

- `<tiny_core/tiny_input.hpp>`: pointer, gesture and time vocabulary, plus `View_context`,
  `Draw_context`, `Scroll_data`, `Draw_callback` and `Notify_callback`, all from
  `tiny_view.hpp`.
- `<tiny_core/tiny_notifications.hpp>`: `Set_param`, `Dark_mode_changed`,
  `Host_preset_loaded` and `Host_event`, from `tiny_events.hpp`.

If you included `tiny_view.hpp` or `tiny_events.hpp` only for those types, include the core
header instead.

### 3. Model headers include core only

`source/models/*.hpp` are included by the generated `<tiny_models.hpp>`, which the framework
includes **before** its own model-aware headers. A model that includes
`<tinyplug/tinyplug.hpp>` creates an include cycle.

```cpp
// BEFORE
#include "tinyplug/tinyplug.hpp"
// AFTER
#include <tiny_core/tiny_core.hpp>
```

Your processor, editor and worker headers keep including `<tinyplug/tinyplug.hpp>`, which now
brings in your models for you. Explicit `#include "models/params.hpp"` lines there are
harmless, but you no longer need them.

### 4. The editor moves to `tiny::edit`

```cpp
// BEFORE                               // AFTER
namespace tiny::plugin {                namespace tiny::edit {
class Editor { ... };                   class Editor { ... };
}                                       }
```

Framework types are still in `tiny`, so they resolve unqualified inside `tiny::edit` just as
they did inside `tiny::plugin`. Rename the namespace in `editor.hpp` and `editor.cpp` and
you're done.

### 5. The worker's channel shape moves out of the class

The four channel variants and the tuning constants were nested in `Worker::Model`. That put
them in a header that needs the framework, so the framework could only read them through the
"must come last" include rule. They now live in a core-only model:

```cpp
// source/models/work.hpp  (NEW)
#pragma once
#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Tick { int64_t sample_pos{}; };
// ... your other message types ...

struct Work {
    using From_processor = std::variant<Tick>;
    using From_editor    = std::variant<Set_session>;
    using To_processor   = std::variant<Set_counter>;
    using To_editor      = std::variant<Session_path>;

    static constexpr auto inbound_capacity  = size_t{64};
    static constexpr auto outbound_capacity = size_t{16};
    static constexpr auto update_period = std::chrono::milliseconds{16};
};

} // namespace tiny::models
```

```cpp
// source/worker.hpp
// BEFORE                                          // AFTER
namespace tiny::plugin {                           namespace tiny::work {
class Worker {                                     class Worker {
    struct Model { ... };                              // (gone — see models/work.hpp)
    using From_processor = Model::From_processor;      using From_processor = User_work::From_processor;
    explicit Worker(Worker_reply_actor<Worker>,        explicit Worker(Worker_replies,
                    Task_manager::Actor);                              Task_manager::Actor);
```

Everywhere else:

| Before | After |
|---|---|
| `plugin::Worker::Model::To_processor` | `User_work::To_processor` |
| `Worker::Model::To_editor` | `User_work::To_editor` |
| `plugin::Tick` (message types) | `models::Tick` |
| `Worker_reply_actor<Worker>` | `Worker_replies` |
| `No_worker` | `work::None` |

`Worker_processor_actor`, `Worker_editor_actor`, `bind_worker`, `handle_worker_reply` and
`on_worker_reply` are unchanged.

The two files are paired. `worker.hpp` without `models/work.hpp`, or the reverse, is a
configure error.

### 6. Aliases you get for free

The generated headers declare these in `tiny`, so you can delete your local copies:

```cpp
using User_params = params::Infos<models::Resolved::Params>;   // <tiny_models.hpp>
using User_meters = meters::Infos<models::Resolved::Meters>;
using User_work   = models::Resolved::Work;
inline constexpr bool has_params, has_meters, has_state, has_work;

using User_processor = plugin::Resolved::Processor;             // <tiny_plugin.hpp>
using User_editor    = plugin::Resolved::Editor;
using User_worker    = plugin::Resolved::Worker;
inline constexpr bool has_editor, has_worker;
```

A redeclaration as a class member (`using User_params = params::Infos<models::Params>;`) is
still legal, because it names the same type. Delete it anyway.

### 7. Meters are optional

A plug-in without `models/meters.hpp` carries no meter machinery at all: no publisher,
mailbox, output parameters (VST3) or Direct Data traffic (AAX). If your meter model declares
only `Num_meters`, delete the file and remove it from your `target_sources`.

Keeping an empty model still works, but the machinery stays compiled in.

Without a meter model, two fields don't exist:

| Field | Absent when `TINY_HAS_METERS == 0` |
|---|---|
| `process::Dsp_context::meters` | the processor has nowhere to write, and nothing to write |
| `Ui_receiver::read_meters` | wrapper-internal; only matters if you build your own `Ui_receiver` |

`Plugin_state::processor_state.meters` is still there, as an empty span, because
`Processor_state` lives in core and can't depend on your models. Code shared between
plug-ins with and without meters can guard itself with `#if TINY_HAS_METERS` or, inside a
template, `if constexpr (has_meters)`.

### Traps

- **A model header that includes `<tinyplug/tinyplug.hpp>`.** While the model only uses
  core types, this is a harmless cycle. Once it reaches for an interface type you get
  `no type named '...' in namespace 'tiny'`, because the interface isn't declared yet at that
  point. Include `<tiny_core/tiny_core.hpp>`.
- **`<tiny_plugin.hpp>` is for wrappers.** Your own code never needs it. It includes your
  `processor.hpp`, `editor.hpp` and `worker.hpp`, so including it from one of those creates a
  cycle.
- **`process::Some_plug_processor` is now asserted.** The generated `<tiny_plugin.hpp>`
  checks it. A processor that was never asserted and doesn't quite match the concept now
  fails to compile, with the diagnostic in the generated header.

### Verifying

Build every format for the plug-in. There is no runtime behaviour change. The only
generated-code difference is that `action_queue` and `undo_history` are now inlined into
each plug-in instead of linked from the static library.

---

## Meter model

Breaks from the model layer. The meter model no longer needs an `Address` enum, and the
publisher and mailbox move into `<tiny_core/tiny_meters.hpp>`.

### 1. Declare `num_meters`, take a raw address

```cpp
// BEFORE                                          // AFTER
struct Meters {                                    struct Meters {
    enum class Address : uint32_t {                    enum class Address : uint32_t { // optional, yours
        Peak_in,                                           Peak_in,
        Num_meters                                         Num_meters
    };                                                 };
                                                       static constexpr auto num_meters = enum_raw(Address::Num_meters);

    static auto make_spec(Address a)                   static auto make_spec(std::uint32_t a)
        -> meters::Spec;                                   -> meters::Spec; // switch (static_cast<Address>(a))
};                                                 };
```

Addresses are `0..<num_meters`. The framework no longer reads your enum, but deriving the
count from it, as above, keeps the two in step. (`std::uint32_t{Address::Num_meters}`
doesn't compile, because a scoped enum won't brace-convert. Use `enum_raw`.) If you switch on the cast
address, end with `case Address::Num_meters: default: return {};`. The value is a raw
integer, and the wrappers build with both `-Wswitch-enum` and `-Wswitch-default`.

### 2. `User_meters` is unchanged

`User_meters` is still `meters::Infos<Model>`, and `num_meters`, `spec(i)` and `specs()`
behave as before. `Infos` caches the specs once. `Publisher` and `Mailbox` are now
templated on the model itself rather than on `Infos`, which only matters if you
instantiate them yourself: write `meters::Publisher<models::Resolved::Meters>`.

`make_spec` is called directly on the audio thread every block, so keep it a cheap,
side-effect-free switch. No allocation, no strings built on the fly.

### 3. Headers and policy order

`<tiny_core/meter_mailbox.hpp>` and `<tiny_core/meter_publisher.hpp>` are gone. Include
`<tiny_core/tiny_meters.hpp>`, or just the core umbrella. `meters::Sample` is gone too:
`Mailbox::read` fills a `std::span<float>` with display-ready values.

`meters::Policy` is now `{Stream, Peak, Trig}` (it was `{Peak, Stream, Trig}`). If you
stored a policy as an integer anywhere, remap it. The framework never persists one.

`Spec` and `Range` no longer define `operator==`.

### Trig semantics

What an editor sees is unchanged: a `Trig` shows its magnitude on the frame it fired and
zero otherwise. Internally the mailbox now clears the value on read instead of counting
triggers, so the per-frame trigger count that `meters::Sample::triggers` exposed is gone.
Several triggers within one frame read as the latest one.


---

## Blocks

Adds `blocks::Model`: typed, trivially copyable frames from processor to editor, for
spectra, scopes and similar. Latest wins. Opt-in: a plug-in without
`source/models/blocks.hpp` is unaffected, apart from the two breaks below. Design in
[plans/block-output.md](plans/block-output.md). [examples/block_demo](examples/block_demo/)
is the reference.

### Breaks

- **`Processor_state` moved** from `<tiny_core/tiny_utils.hpp>` to `<tinyplug/tiny_events.hpp>`,
  because it now names model types. If you include the umbrella you won't notice.
- **`Processor_state::meters` exists only with a meter model**, like
  `Dsp_context::meters`. Code that reads it in a meter-free plug-in must go.
- **`view_impl::run_frame` takes the editor's retained frames** after `_ui_meters`
  (`blocks::Frames<models::Resolved::Blocks>&`). Only a custom wrapper calls it.

### Adding blocks

```cpp
// source/models/blocks.hpp — core only
struct Spectrum_frame { std::array<float, 1024> db{}; std::uint32_t used{}; };
struct Scope_frame    { std::array<float, 512> samples{}; };

struct Blocks {
    using Types = std::variant<Spectrum_frame, Scope_frame>;   // each trivially copyable
    enum class Address : std::uint32_t { Spectrum, Scope, Num_blocks };
    static constexpr auto num_blocks = enum_raw(Address::Num_blocks);

    static constexpr auto make_spec(std::uint32_t a) -> blocks::Spec  // must be constexpr
    {
        switch (static_cast<Address>(a)) {
            case Address::Spectrum: return {blocks::kind_of<Spectrum_frame, Types>};
            case Address::Scope:    return {blocks::kind_of<Scope_frame, Types>};
            case Address::Num_blocks:
            default:                return {};
        }
    }
};
```

Processor, through `Dsp_context::blocks`:

```cpp
auto& frame = context.blocks.write<Address::Spectrum>(); // Spectrum_frame&
// ... fill it ...
context.blocks.publish<Address::Spectrum>();              // sent at the end of this block
```

The staging frame persists between publishes. It's copied out at the **end** of the
block, so don't overwrite it after publishing in the same block. Keep your own buffer if
you might (block_demo's scope does). A write without a publish sends nothing. Nothing is
sent during an offline bounce.

Editor, through `Plugin_state::processor_state.blocks`:

```cpp
const auto& frames = state.processor_state.blocks;
draw(frames.latest<Address::Spectrum>());                  // always answers
if (const auto* f = frames.fresh<Address::Scope>()) ...    // null unless new this draw
```

Before the first publish, `latest` returns a value-initialised frame.

Per format, frames arrive at up to the draw rate in CLAP, AUv2 and AUv3, up to 60/s in
VST3 (one `IMessage` per changed address), and ~33/s in AAX (Direct Data).

---

## Change sets

`Change_list` is gone; `Change_set<Event, N, Producers>`
([change_set.hpp](libs/tiny_core/include/tiny_core/change_set.hpp)) replaces it, and VST3's
state loads use it instead of `Overwrite_queue` (which stays in `lock_free_queue.hpp`, unused). `N` is the
address count (usually `User_params::num_params`), fixed at compile time. `push`, `push_n` and
`consume` are unchanged, except that `consume` now returns whether anything arrived. Only wrapper
code used `Change_list`.

---

## State

Adds `state::Model`: one trivially copyable struct, declared in `source/models/state.hpp`,
that the editor and (optionally) the processor both edit, synchronized in every format
and undoable alongside parameters. Opt-in; a plug-in without it is unaffected.
[examples/state_demo](examples/state_demo/) is the reference. The document is saved with
the session and in presets in every format (design:
[plans/state-persistence.md](plans/state-persistence.md)).

### Breaks

Only for code that builds its own `Ui_receiver` or drives `Undo_history` directly:

- `Ui_receiver` gains `sync_state` (with a state model), after `action_handler`.
- `Undo_history` steps can now hold a document change. `perform_actions` commits any
  uncommitted document edit before it undoes or redoes, and state steps are capped by a
  byte budget (`set_state_budget`, 16 MB default), evicting the oldest steps first.
- `State_adapter::Save_model` gains `state_record`, and `State_adapter` gains `state_record(json)`.
- Editor state keys starting with `tinyplug-` are reserved: they assert in debug builds and
  are dropped when a session is saved.

### Declaring a document

```cpp
// source/models/state.hpp — core only
struct State {
    static constexpr auto writers = state::Writers::Editor;   // or Processor, Both
    std::array<std::uint8_t, 16> level{};
};
static_assert(state::Model<State>);
static_assert(state::byte_comparable<State>); // recommended: no padding, no floats
```

Requirements: trivially copyable, default constructible, `alignof <= 8`, no pointers
or handles, since it crosses a process boundary in VST3. Keep it under 64 KB, a soft limit:
every cost scales with `sizeof(State)`, not with how much changed. Every undo step holds two
copies, so depth is about `set_state_budget / (2 × size)`: 128 steps at 64 KB. In the
distributed formats every edit carries two copies, and while the processor writes, every
snapshot carries one, up to 60 a second. Past 64 KB it still works, but undo gets shallower
and the traffic heavier; size capacity to what's used (pooled lists rather than per-slot
maximums). Session size is separate: it's whatever `save` writes.

`writers` decides what exists:

| `writers` | Editor | Processor |
|---|---|---|
| `Editor` | `edit` | `get` only |
| `Processor` | undo/redo only | `get`, `mutate` |
| `Both` | `edit` | `get`, `mutate` |

### Editor

Through `Edit_context::state`:

```cpp
const auto& doc = _edit.state.view();                          // draw from this
_edit.state.edit([col, level](State& s) { s.level[col] = level; });
_edit.state.commit();                                          // gesture end: one undo step
```

The lambda is **kept and re-run** if the processor refused it or a snapshot arrived
while it was outstanding. So capture by value, and read anything you need from the
`State&` it's given, not from a copy taken beforehand. `edit`'s second argument is the
policy for a processor that also writes (`Retry` by default; `Merge`, `Overwrite`); it
has no effect under `Writers::Editor`.

`commit` waits until the edit is confirmed, then records the step. Call it at gesture
end and at other natural boundaries. Undo and redo go through the existing
`_edit.undo_redo`, so parameter steps and document steps share one history.

Under `Both` or `Processor`, anything the processor wrote since the last step becomes a
step of its own at the next commit point, so a recording pass is undoable. A write that
lands while an undo is still being sent survives it. The same applies to anything the
processor computes and keeps: a learned profile or calibration is undoable too, and an undo
after it takes the result back.

### Processor

Through `Dsp_context::state`, valid for this block only:

```cpp
const auto& doc = context.state.get();
context.state.mutate().recorded[i] = note; // Processor / Both only
```

An editor edit lands at the start of a block, never in the middle of one.

### Persistence

Sessions and presets store the document as a record the framework wraps around your
payload. Declare the payload with a `save`/`load` pair, and own its versioning:

```cpp
static auto save(state::Writer out, const State& value) -> bool
{
    return out.write(std::uint32_t{2}) && out.write(value.level) && out.write(value.swing);
}

static auto load(state::Reader in, State& value) -> bool
{
    auto version = std::uint32_t{};
    if (!in.read(version) || version > 2) return false;   // from a newer build: keep the default
    if (!in.read(value.level)) return false;
    return version < 2 || in.read(value.swing);           // v1 sessions: swing keeps its default
}
```

- `load` fills a default `State`; only a `true` return is used. Anything else, including a
  session saved before the document existed, loads the default.
- `write`/`read` take trivially copyable values (scalars, enums, `std::array`s, plain
  structs) and store them little-endian. A nested struct's layout becomes part of the format.
- Declare both or neither. With neither, the whole struct is stored raw and loads only while
  `sizeof(State)` is unchanged. Add the pair later and branch on `in.raw()` to read those.
- Debug builds check every save by loading it and saving again; a field missed or read out
  of order asserts the first time state is saved.
- **Whatever `load` accepts is permanent.** Once a version has shipped, keep reading it.

A host load (session, preset) is one undo step covering its params and the document. An
editor-side preset browser loads the record as an ordinary edit, through
`_edit.state.load_record(_edit.state_adapter.state_record(json))`.

---

## Notes

Adds notes in and out: instruments, note effects, and effects that take or send notes.
Declared in CMake, not as a model. [examples/sine_synth](examples/sine_synth/) and
[examples/step_sequencer](examples/step_sequencer/) are the references; the design, and
why CC is a closed set, is [plans/midi-support.md](plans/midi-support.md).

### Breaks

- **`process::Some_plug_processor` is now `process::Interface`.** Rename your
  `static_assert`. Its requirements now follow what the plug-in declares: with notes in it
  requires `handle(const Note::Any&)` and `handle(const Control::Any&)`, and a work model
  that replies to the processor requires `handle_worker_reply`. The generated
  `<tiny_plugin.hpp>` names the missing member.
- **`TINY_AUV2_TYPE` is derived.** Delete it, or keep a value that matches; a contradicting
  one is a configure error.
- **`TINY_PLUGIN_WANTS_SIDECHAIN` is deprecated** in favour of
  `TINY_PLUGIN_WANTS_AUDIO "in;out;sidechain"`. It still works.
- Wrapper code only: `process::Tagged_event::event` is a `process::Input` (parameters,
  notes or controls) with an `order` field, delivered through `process::deliver`, and sorted
  with `process::before`.

### Declaring

```cmake
add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_AUDIO "out")   # in, out, sidechain, or none; default "in;out"
add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_NOTES "in")    # in, out, expression, or none; default none
```

Audio in and out is an effect (notes optional); audio out with notes in is an instrument (a
sidechain optional); no audio with notes in and out is a note effect. Each format's type and
category follow; `TINY_VST3_SUBCATEGORIES`, `TINY_CLAP_FEATURES` and `TINY_AAX_CATEGORIES`
still add descriptors. An AAX note effect is a `MIDIEffect` that passes audio through, as Pro
Tools requires of MIDI effects.

For Live, which loads neither `aumi` AUs nor VST3 effects without an audio input, a note
generator declares `TINY_PLUGIN_WANTS_AUDIO "out"` with notes in and out: an instrument that
emits notes, routed to another track with "MIDI From".

`expression` (with `in`) declares MPE and per-note expression to every host that asks, and
reads MIDI 1.0 member channels as MPE: their bend, pressure and CC 74 arrive as
`Note::Expression{Tuning, Pressure, Brightness}`, only the manager channel as `Control`. A
note starts at `Expression::neutral` for every kind. For a player-facing MPE switch, answer
`auto mpe_enabled() const -> bool` (from a parameter, say); without it MPE is always on. The
switch changes only how MIDI 1.0 is read; typed expressions from VST3 and CLAP hosts arrive
either way.

### Processor

```cpp
auto handle(const Note::Any& note) -> void;       // On, Off, Choke, Expression
auto handle(const Control::Any& control) -> void; // Bend, Pressure, Pedal
```

Match notes on `note.id` alone: the framework mints one id per note whatever the format
supplied. `Reset::Hard` also means release every voice. With notes out, send through the
context, with frames counted from the start of this `process` call and ids of your own:

```cpp
context.notes.send(frame, Note::On{{.id = _next_id++, .channel = 0, .key = 60}, 0.8f});
```

### Editor

Beyond notes and controls, `context.notes.send(frame, midi::Raw::cc(channel, number, value))`
(or `Raw::program`, or any channel voice message as three bytes) sends MIDI exactly as written,
for a device downstream. Output only, and no SysEx yet.

The types live in `tiny::midi` (`<tiny_core/tiny_midi.hpp>`); `tiny::process` re-exports `Note`,
`Control` and `Performance`, so processor code writes them unqualified.

Not included, in or out: SysEx, system messages (clock, song position, start/stop), MIDI 2.0,
and MPE out. On the way in, arbitrary CC, NRPN and program change are dropped:
the host maps controllers to parameters. The full list is "Not included" in
[plans/midi-support.md](plans/midi-support.md).

`_edit.notes.send(Note::Any / Control::Any)` plays the processor, e.g. an on-screen
keyboard, landing at the next block. Give notes an id of your own (non-zero) to tell two
fingers on one key apart, pair every `Off` with its `On`, and release held keys in
`on_gui_hide`.


---

## Task lifetimes

`Task_manager` work can no longer outlive the plug-in. The host owns the plug-in and may
destroy it at any time, but a dialog sheet, a network completion or a queued background task
used to hold a raw `Task_manager*` and call into it (and into whatever `this` its task captured)
after the instance was gone. Now:

- `Task_manager::Actor` holds a weak reference to a shared core. Posting after the manager has
  shut down, or been destroyed, is refused: `on_main`/`on_background`/`on_serial` return `false`
  and the task is dropped without running. An actor is safe to copy into anything.
- `Task_manager::shutdown()` refuses new work, discards queued work without running it, and waits
  for any task already running, on any lane, including a main task when the host destroys us off
  main. Every wrapper calls it first in its destructor, before the editor and worker die.
- The main queue has no fixed size (it was 16 slots, with an assert on overflow).
- `Actor::is_open()` tells a long-running task that shutdown has begun.
- AUv3's `Task_manager` moved from the view controller to the audio unit, beside the editor it
  serves: the editor outlives the view there, so its actor used to dangle. The AUv3 worker moved
  out of `DSPKernel` onto the audio unit too, as in every other wrapper, and now gets a working
  task actor. It used to get an empty one, so its `on_main`/`on_background`/`on_serial` did
  nothing on AUv3.

### Breaks

Nothing fails to compile. `Actor{ptr}`, `Actor{nullptr}` and `Actor{}` all still work, and the
`on_*` calls now return `bool`, which existing call sites ignore. Behaviour changes in two places:

- Background and serial work still queued at teardown is discarded. It used to run inside the
  destructor, after the queues it might post to were already gone.
- `task_manager.hpp` no longer includes `serial_queue.hpp`, `task_launcher.hpp`,
  `notification_queue.hpp` or `lock_free_queue.hpp`. Include them directly, or use
  `<tiny_core/tiny_core.hpp>`, which now includes all four.

### What client code should do

The licensing code in `all_plugins/shared` is the worked example throughout.

1. **Reach `this` only through the actor.** An OS callback (an `NSURLSession` completion, a
   dialog sheet, `dispatch_after`, a WinHTTP callback) must post to the actor and do its work in
   the task, never call a captured `this` directly. Tasks may capture `this` freely: shutdown
   runs before the editor or worker dies, and a refused task never runs. `Networking::get_async`
   and `post_async` already follow this, so `License_checker`'s `[this]` completions are safe.
2. **Don't sleep on a lane.** `with_delay` in `license_checker.cpp` sleeps on the background lane.
   That lane has one thread, so every other background task, dialog callbacks included, waits
   behind the sleep, and shutdown waits for it too, which stalls the host's teardown for up to
   the poll interval. On Apple, use a timer that posts through the actor:

   ```cpp
   static auto with_delay(double seconds, std::function<void()> callback, Task_manager::Actor tasks) -> void
   {
       const auto when = dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(seconds * NSEC_PER_SEC));
       dispatch_after(when, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
           tasks.on_background(callback); // Refused, and dropped, once the plug-in is gone.
       });
   }
   ```

   On Windows, don't leave an OS timer pending: a host may unload the DLL. Wait in short slices
   instead, so teardown waits one slice at most:

   ```cpp
   tasks.on_background([=] {
       for (auto waited = 0.0; waited < seconds; waited += 0.05) {
           std::this_thread::sleep_for(std::chrono::milliseconds{50});
           if (!tasks.is_open()) return;
       }
       callback();
   });
   ```

3. **Bound blocking work.** `networking_win.cpp` runs the whole request as one background task,
   and shutdown waits for it. WinHTTP's defaults allow a minute or more, so set
   `WinHttpSetTimeouts` (a few seconds each) to bound how long a teardown can hang.
4. **Treat `false` as "gone", not as an error.** A refused post means the plug-in is being
   destroyed. Don't assert on it, and don't retry.
5. **If you own a `Task_manager`** (a controller that isn't the wrapper's), call `shutdown()`
   first in your destructor, before anything its tasks capture is destroyed.
6. **Optional:** keep the `NSURLSessionDataTask` and `cancel` it in the owner's destructor. It
   isn't needed for safety (a late completion is refused) but it stops wasted network work.

Main tasks still run only while the editor draws, so keep `Delivery::Background` for work that
must progress with the window closed, like the activation poll. Dialog callbacks may capture
`this` too: after shutdown a late answer is dropped instead of delivered.

---

## Maximum latency

The bypass delay that compensates a plug-in's own latency is now sized once, at `configure`,
and never grows on the audio thread. A processor whose latency changes at runtime must say how
far it can go:

```cpp
auto max_latency_samps() const -> uint32_t; // Longest latency this configuration can propose.
```

Compute it from the sample rate in `configure`, not from live parameters: it must hold until the
next `configure`. Without it the latency is treated as fixed at what `configure` came up with. Any
integer return type is accepted.

**This one does not break the build.** A plug-in that proposes more than its maximum hits an
assert in debug builds; in release the proposal is refused, so the host is never asked and the
kernel never receives `Reset::Latency`. Check every processor that sets
`Dsp_context::propose_latency`. [examples/latency_demo](examples/latency_demo/) is the reference.

Also fixed along the way: a latency that was an exact power of two used to wrap the bypass delay
line to zero, so bypass had no compensation at those values.
