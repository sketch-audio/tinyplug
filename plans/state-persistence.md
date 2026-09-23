# Plan: Persisting the state document

> Status: **built, steps 1–6.** The runtime half of `state::Model` had landed: sync in all
> five formats, undo/redo, and `examples/state_demo`. This is the other half: sessions,
> presets and host loads, with maximal backward compatibility. All five formats, preset JSON
> and both exporters write and read the record; host loads are one undo step. Step 7, the
> lockfile, is not started. Sections below describe what was built where it differs from
> the first design.

## Goals

1. **Old sessions load in new builds.** A session saved before a plug-in had a state
   model, or before this framework version, loads with every parameter intact and the
   document at its default.
2. **New sessions degrade in old builds.** An older build of the same plug-in loads a
   newer session with its parameters intact and ignores the document. It never
   misreads it.
3. **A document never reaches the processor half-loaded or misread.** Only a `load` that
   returns `true` on a bounds-checked reader replaces the document; anything else keeps
   the default.
4. **The author owns versioning.** `save`/`load` decide what each version means; the
   framework only guarantees the container and the safety rails.
5. **One record, every container.** The same bytes go in the CLAP stream, the VST3 chunk,
   the AU dictionary, the AAX chunk and preset JSON. Only the container differs.

## The record

The framework owns the **container** and the author owns the **payload**. The author
serializes field by field in two static functions, and decides what a version means:

```cpp
struct State {
    static constexpr auto writers = state::Writers::Processor;

    std::array<std::uint8_t, 64> midi_notes{};
    std::uint8_t swing{};                          // added in v2

    static auto save(state::Writer out, const State& value) -> bool
    {
        return out.write(std::uint32_t{2})         // the author's own version
            && out.write(value.midi_notes)
            && out.write(value.swing);
    }

    static auto load(state::Reader in, State& value) -> bool
    {
        auto version = std::uint32_t{};
        if (!in.read(version) || version > 2) return false;  // from the future: keep the default
        if (!in.read(value.midi_notes)) return false;
        if (version >= 2 && !in.read(value.swing)) return false;
        return true;                                         // v1: swing keeps its default
    }
};
```

What the framework wraps around the payload:

```
State_record (little-endian)
  u32  magic           'tSTA'
  u32  record_version  framework container version, 1
  u32  kind            0 = the author's save(), 1 = the raw fallback below
  u32  payload_length
  u8[] payload         whatever save() wrote
```

### Why this is safe

The functions are free-form, but everything that can go wrong is bounded by the
framework:

- **`Reader` is bounds-checked.** Every `read` returns false instead of running past
  `payload_length`, so a truncated or foreign payload can't overread. `read` also
  returns false once the reader has failed, so a chain of reads stops at the first
  failure.
- **`load` fills a scratch copy.** The framework calls `load(reader, scratch)` on a
  `T{}`. Only a `true` return reaches the processor, through `on_session_load`. A
  `false` keeps the default and logs, so a half-loaded document can't happen. A load
  that returns `true` with payload bytes unread asserts in debug builds (save and load
  disagree) and is accepted in release, so the two builds load the same sessions.
- **`Writer` and `Reader` are handles.** They point at a framework-owned `Payload_out` /
  `Payload_in` (the actor pattern), which holds the buffer and the cursor, so the framework
  sees how far a load read however the author passes the handle.
- **Fixed encoding.** `Writer` and `Reader` accept only trivially copyable values (scalars,
  enums, `std::array`s and plain structs of them) and write them little-endian, so a
  session moves between machines. Reading into a whole nested struct is allowed, but
  then its layout is part of the format.
- **Off the audio thread, allocation allowed.** `save` runs where the host asks for
  state; `Writer` appends to a buffer the framework owns.
- **A round-trip check in debug builds.** After every `save`, the framework `load`s the
  payload into a scratch `T`, saves that again, and compares the two payloads. A field
  written by `save` but dropped by `load`, or read in a different order, trips an assert
  the first time state is saved, not when a user reopens a session. Comparing payloads
  rather than structs keeps it valid for floats and padding. It can't see a field that
  neither function mentions; that's what the permanence check below is for.

### Why it's better than a raw copy

A raw `memcpy` with a version number ties the format to `sizeof(T)` and field order.
Every edit to the struct needs a version bump and a migration, and a forgotten bump
misreads a session. Here the struct can change freely (reorder, pad, add fields
anywhere) while the format stays whatever `save` writes. The common evolution, a new
field with a default, is one line in each function: old payloads simply lack it.

### When the author declares neither

`save` and `load` are optional **as a pair**; declaring one without the other is a
`static_assert`. With neither, the framework writes the whole of `T` as a `kind = 1`
record and loads it only if the length is `sizeof(T)`, else keeps the default. Fine for
prototyping and for a document that will never change.

When the layout does change, the author adds the functions, and old raw payloads stay
readable: `Reader::raw()` is true for a `kind = 1` record, so `load` can take a
dedicated branch that reads the old layout (a struct the author keeps for exactly that).
Nothing is guessed from the payload's contents.

### Permanence

Whatever `load` accepts is a permanence surface, like the parameter address,
identifier and `au_order()`: once a payload shape has shipped, `load` must keep reading
it. The debug round trip catches asymmetry within one build, but not a dropped branch
for an old version. That belongs in the `params.lock` work
([param-lockfile.md](param-lockfile.md)) as a checked-in golden payload per shipped
version that the next build must still load.

## Where it goes in each container

Every current reader stops once it has read the counts in its header and ignores
anything after (checked: CLAP `_read_state_chunk`, VST3 `Audio_effect::setState`). The
dictionary- and key-based formats ignore keys they don't know. So every placement below
is invisible to old builds.

| Format | Placement | Old build | New build, old session |
|---|---|---|---|
| CLAP | append the record after the editor pairs in the one stream | stops at the pairs | stream ends: default |
| VST3 | append to the **processor** chunk (`getState`), after the bypass float | stops after bypass | stream ends: default |
| AUv2 | new dictionary key `tinyplug-state`, `CFData` | unknown key | missing key: default |
| AUv3 | same key in `fullState` | unknown key | missing key: default |
| AAX | new chunk key `tinyplug-state`, **base64 string** (`AAX_CChunkDataParser` has no binary type) | unknown key | missing key: default |
| JSON presets | top-level `"state": "<base64 record>"` | `State_adapter` reads only the keys it knows | missing key: default |

AAX string length is not a constraint. `AAX_ChunkDataParserDefs::MAX_STRINGDATA_LENGTH`
(255) is declared but never used: `AAX_CChunkDataParser` sizes a string by `Length()` when
it builds a chunk and reads to the NUL when it loads one, and `AAX_CString` wraps a
`std::string`. The editor-state strings already go through it. Base64 contains no NUL.
There's no size cap on `T`. Large documents cost copies and undo memory, so size is
advisory, not enforced. Still open: whether any host truncates a VST3 chunk it considers
oversized.

**Reserved keys.** The dictionary- and key-based containers (AUv2, AUv3, AAX) hold the
editor `State_map` in the same key space as the framework's own keys; AAX writes editor keys
into the chunk by name. The `tinyplug-` prefix is the framework's: an editor key using it
asserts in debug builds and is dropped in release.

VST3's controller chunk doesn't change. The controller receives the document through
`setComponentState`, which already gets the processor's chunk, and seeds its
`Editor_link` from that. No extra hop.

## Load: restore entry points

Every format already has one restore path per host load (see CLAUDE.md, "Host-initiated
preset/state loads"). Each gains the same steps, after `push_host_load` so the document
lands in the load's undo step:

```cpp
const auto doc = state::decode_record_or_default<M>(record);   // T::load into a scratch copy, or default
_state.on_session_load(doc);                                   // processor copy, Overwrite
_state_link.load(doc);                                         // editor copies, baseline, undo step
```

- **Coupled formats** do both in the restore call: CLAP `_update_state`, AUv2 `RestoreState`
  and `_update_state` (factory presets), AUv3 `setFullState` (factory presets arrive there too).
- **VST3**: the processor's `setState` loads its copy, under the mutex it shares with the
  snapshot relay and `getState`; the controller's `setComponentState` reads the same chunk and
  calls `load`.
- **AAX**: the algorithm doesn't see `SetChunk`, and the data model only flushes while the GUI
  is open. So `SetChunk` loads with `Resend::Yes`, empties the outbox (the load supersedes a
  waiting edit) and flushes at once: the whole document goes out as an `Overwrite` patch. A
  reset before it lands seeds the algorithm from `Reset_state`, which already carries `sent()`.

Two runtime fixes this needed:

- **An `Overwrite` replaces whatever is staged.** `Store::load_bytes` used to merge an
  incoming patch into a staged replace, which for an `Overwrite` whose base didn't describe
  that replace left stray bytes behind.
- **A load skips a stale snapshot.** `on_session_load` marks the snapshot published before it
  as sent, so a pump that runs after the editor reseeded can't put the old document back. A
  block racing the load can still publish one; the next block's snapshot corrects it.

**Undo.** `Editor_link::load` diffs the view against the loaded document and calls
`Undo_history::amend_host_state`, which folds `{view, doc}` into the step `push_host_load`
opened, or opens one when the load moved no params. `add_param` markers from the editor's
`notify` fold into the same step. Reseeding resets the link's baseline, so the load isn't
recorded twice. An identical load records nothing.

AUv2's factory-preset path had no host-load step at all; it now opens one, so its document
can't fold into an earlier load's step. It still doesn't `notify` the editor.

## Save: who reads the document

- **Sessions, coupled formats and VST3:** `Processor_side::snapshot()`, whatever `writers` is.
  It answers from a staged edit if one is pending, and under `Writers::Editor` it trails the
  view by at most the last frame's flush (`run_frame` flushes at the end of every frame).
  Coupled formats read on the main thread, the same thread as the pump. VST3's relay `pump`,
  `getState` and `setState` share `_state_mutex`.
- **Sessions, AAX:** the data model has no processor to read, so it saves
  `Editor_link::view()`: the editor's copy, including edits not yet confirmed. Under
  `Processor`/`Both` it can trail the algorithm by one Direct Data wakeup (about 30 ms), as the
  params path does.
- **Presets** (`State_adapter::Save_model::state_record`): `Editor_link::view()` in every
  format, since the editor is what saves them.

## Presets

- `State_adapter` writes `Save_model::state_record` as top-level `"state"` (base64) and reads
  it back with `state_record(json)`, which is empty for a missing or malformed value. It never
  sees `T`: core is compiled once, so the record stays opaque bytes there.
- Factory presets need no build change: the JSON carries the record.
- The exporters pass the JSON's record through untouched, so they can't drift from the
  wrappers. The `.tfx` gets it under `tinyplug-state`. The `.vstpreset` processor chunk never
  had a bypass float, so it writes `no_value` as a placeholder before the record, and every
  VST3 reader now leaves the bypass alone on `no_value`.
- An **editor-authored** preset browser calls `Editor_actor::load_record(record)`, an
  `Overwrite` edit that undoes like any other.

## Order of work

Steps 1–6 are done; 7 is not started.


1. `state::Writer` / `Reader` and `encode_record` / `decode_record` in core: detecting
   `save`/`load`, the raw fallback, and the debug round trip. Tests for truncated payloads,
   a load returning false, a payload from a future version, and the fallback-to-functions
   upgrade. No wrapper changes.
2. JSON presets: `State_adapter` read/write. Test old-without-state and
   new-with-future-version.
3. CLAP, then AUv2 and AUv3: the coupled formats, one restore and one save each.
4. VST3: processor chunk, `setComponentState` seeding, and the relay/getState mutex.
5. AAX: chunk key, base64, `CompareActiveChunk` covering the document so Pro Tools'
   compare light sees state edits.
6. Host-load undo: `amend_host_state`, folded into `push_host_load`'s step.
7. Lockfile: golden payloads for each shipped shape.

## Open questions

- **P1.** Closed: a refused or mismatched record keeps the default and logs, in release too.
  Refusing the whole session is worse than losing the document.
- **P2.** Closed: no `read_or`. A version is one line, and without it missing and corrupt
  look the same.
- **P3.** Closed: AAX has no string ceiling (see above), so no compression.

## Known limits

- **VST3, an edit crossing a load.** An edit sent by the controller before the host calls
  the processor's `setState`, and delivered after it, is applied on top of the loaded
  document. `Retry` edits will usually be refused as drifted; `Merge` edits land. The window
  is one `IMessage`, and only VST3 has it. A load epoch in the edit tag would close it;
  not done.
- **A preset without `"state"` resets the document.** By design (goal 1), and the same as a
  session: the preset predates the document or its author didn't save one.
- **AUv3 reads the processor copy wherever the host calls `fullState`.** Like the editor
  state it saves beside it, that assumes the main thread. The store's debug reader guard
  asserts if a pump overlaps it.
- **Not checked in a real host** beyond `clap-validator` (all state tests pass on
  `StateDemo.clap`). Worth one pass each through Logic (AUv2/AUv3), a VST3 host that
  reloads a project, and Pro Tools including the compare light.
