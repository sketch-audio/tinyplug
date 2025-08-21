# tinyplug
*Hypermodern C++ Audio Plug-in Framework*

## Features
- Write once, run everywhere.
    - Supported platforms: macOS and Windows.
    - GPU-backed graphics via Google's Skia Library.
    - Supported plug-in formats: AAX, AUv2, CLAP, VST3.
- Supported plug-in types:
    - Stereo in/out effect with optional sidechain.
- Declarative approach throughout.
- Sample-accurate events & automation
    - Tinyplug automatically interleaves host events (including ramps) with calls to your process function for precise automation playback.
- Well-defined latency handling
    - tinyplug checks your plug-in's latency after your kernel is `reset` and reports that to the host.
    - If you want to change the latency mid-stream, your kernel must propose a latency, continue processing, and when the host accepts the new latency, immediately apply changes.
    - Your kernel should make latency proposal and acceptance realtime-safe operations.

## Parameters
- Static parameter model
    - Enumerate parameter identifiers in `Param_id`.
    - Declare parameter infos as a tree of `Param_spec` structs.
    - Parameter identifiers can be used as array indices.
- Type-safe parameter semantics
    - `Bool_semantics`: for "true" or "false".
    - `List_semantics`: for indices in a list.
    - `Int_semantics`: for integers.
    - `Real_semantics`: for continuous values.
- Fully enumerated host policies (no flags)
    | Policy         | Automatable? | Visible? | Persistent?| Note |
    |---------------:|:------------:|:--------:|:----------:|:----:|
    | `automation`   |      ✅      |    ✅    |     ✅     |
    | `control`      |      ❌      |    ✅    |     ✅     | (do any hosts actually support?)
    | `state`        |      ❌      |    ❌    |     ✅     |
    | `interface`    |      ❌      |    ❌    |     ❌     |
- Parameter tree versioning
    - Once shipped, you can add new parameters.
    - New parameters get set to their default values when loading old state.

## Architecture
- Decoupled processor (`Dsp_kernel`) and editor (`Custom_view`)
- Structured communication between processor and editor.


## Style
- Herb Sutter AAA style
    - Most lines of code begin with `const auto`.
    - We also use trailing return types.
    - See: https://herbsutter.com/2013/08/12/gotw-94-solution-aaa-style-almost-always-auto/
- Stroustrup style
    - Snake case.
    - User types begin with a single capital letter.
    - Opening brace on new line only for function definitions.
    - See: https://www.stroustrup.com/Programming/PPP-style.pdf

## Todo
- Tail time?
- Merge various user CMake plug-in codes
- Add AUv3 format for iOS and macOS
- Add Linux support for CLAP, VST3
- Add LV2 format for Linux
- Breakout dependencies project
- MIDI events
- Synth support

## UI stuff 
- Draw timestamp
- Logical & real size?

## Wants
- No shared_ptr
- No singletons
- No raw loops

## Consider
- Use optionals in `Musical_context`

## Needs tested
- Sidechain (add export and visualize?)
- Musical context
- Export types (stream, trig)

## Demo plug-in