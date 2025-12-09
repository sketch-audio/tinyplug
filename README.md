# tinyplug
*Minimal, modern C++ audio plug-in framework*

## Features
Tinyplug is a C++20 audio plug-in framework that makes it easy to write audio plug-ins and build them for different platforms and formats.

- Supported platforms & formats:
    | Platform | AAX | AUv2 | AUv3 | CLAP | VST3 |
    |---------:|:---:|:----:|:----:|:----:|:----:|
    | iOS      |     |      |  ✅  |      |      |
    | macOS    | ✅  |  ✅  |  ✅  |  ✅  |  ✅  |
    | Windows  | ✅  |      |      |  ✅  |  ✅  |
    - *Linux & LV2 support on roadmap.*
- Supported plug-in types:
    - Stereo in/out effect
    - Optional sidechain input
    - Optionally allow mono in/out
    - *Synth support on roadmap.*
- Sample-accurate events & automation
    - Host events (including ramps) are interleaved with calls to your process function so you have full control over automation playback.
- Thread-safe architecture
    - The framework handles communication between your processor and editor using simple message queues.
- GPU-backed graphics via Google's Skia Library.
    - iOS & macOS: Metal
    - Windows: Direct3D 12
    - *Software backend on roadmap*

## Architecture
Your processor (`Plug_processor`) and editor (`Plug_editor`) are fully decoupled. Tinyplug handles communication between these classes and the host via simple message queues. There is no sharing of data between threads. The framework makes the source of truth wherever the plug-in format wants it to be.

### Processor (`Plug_processor`)
- Receives parameter changes on real-time thread.
- Can send data to the UI by writing to an array of meter values.

### Editor (`Plug_editor`)
- On draw, receives a read-only copy of the current parameter and and meter values.
- Controls can set parameter values by sending `User_actions`.

## Parameters
Your plug-in implements a static interface (`Param_model`) where you enumerate the parameter identifiers. You declare your parameter infos as a tree structure. The framework preserves this structure where the format allows (e.g. AUv2 clumps). In practice, parameter values are stored as a flat array and the identifier can be used as an index. There are some restrictions to this approach but they are documented.

Tinyplug provides a structured interface so your parameters always have well-defined semantics.
- Type-safe parameter semantics via `std::variant`
    - `Bool`: for values that are "true" or "false"
    - `List`: for indices in a list
    - `Int`: for integers
    - `Fixed`: continuous-like but with quantized to a fixed step size (ex. 0.5)
    - `Real`: for continuous values
- Fully enumerated host policies (no flags)
    | Policy         | Automatable? | Visible? | Persistent?|
    |---------------:|:------------:|:--------:|:----------:|
    | `automation`   |      ✅      |    ✅    |     ✅     |
    | `control`      |              |    ✅    |     ✅     |
    | `hidden`        |              |          |     ✅     |
    | `interface`    |              |          |            |

## Meters
The processor can send data to the editor by writing to an array of meter values. Your plug-in implements a static interface (`Meter_model`) to tell the framework how many meters you want and how it should process those values for delivery to your editor.

- Use the `Meter_policy` enum to specify how tinyplug should treat your meter values.
    - `peak`: Your editor receives the max unconsumed value.
    - `stream`: Receive the latest value.
    - `trig`: Receive an event exactly once each time it happens.

## Latency
Tinyplug offers well-defined handling of latency changes.
- The framework checks your plug-in's latency immediately after your kernel is `reset` and reports that to the host.
- If you want to change the latency mid-stream, your kernel should:
    - Propose a latency
    - Continue processing
    - Apply the host-accepted latency on receipt of an `Accepted_latency` event
- Your kernel should make latency proposal and application realtime-safe operations.

## Platform library
- Message 
- Confirm
- Text input via dialog box
- Open url in browser
- Dark mode

## Style
The style actually developed over several years of writing C++ alongside Swift and Kotlin. Aim for a terse, readable modern style consistent with other modern languages and the C++ standard library.
- Herb Sutter AAA or "left-to-right" style
    - Most lines of code should begin with `const auto`.
    - Functions should be written with trailing return types.
    - See: https://www.youtube.com/watch?v=xnqTKD8uD64
- Stroustrup style (modified)
    - Snake case
    - User types begin with a single capital letter
    - Opening brace on new line only for function definitions (except ctor/dtor)
        - Try this and you will love it.
    - See: https://www.stroustrup.com/Programming/PPP-style.pdf
- Most member variables are prefixed with an underscore

## TODO
- iOS view resizing lag in Logic.
- Multitouch (Windows)
- Basic control library
- Presets
- Preferences
    - E.g. light mode/dark mode
    - Scopes: manufacturer, plug-in
- System dialogs
    - File load & save
- Licensing helpers
    - System ID, name
- MIDI events
- Synth support
- Add Linux support for CLAP, VST3
- Add LV2 format for Linux

## Test Plug-ins
Tinyplug ships with some test plug-ins that can be useful for making sure things are working as advertised.
### Done (ish)
- Automation Tester
- Gain Demo
- Latency Demo
- Platform demo
### Todo
- Meters Demo
- Musical Context Demo
- Params Demo
    - Semantics
    - Policy
- Sidechain Demo

## Consider
- How to handle logical vs. real size?
- Optionals in `Musical_context`?

## Ideas
- Instead of pop_exports, just have a sync_ui_values where we push everything into the ui before we present.

## CMake Refactorings
- Idea: move platform into tinyplug proper?
- TINY_AUV2_TYPE -> TINY_AU_TYPE
- TINY_PLUGIN_CODE for AUv3 same as AUv2?

## Build
- Be sure to also set `-DTINY_DEPS_PATH=../tiny_deps`
- Unix makefiles: 
    - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug|Release`
    - `cmake --build build`
- iOS AUv3: `cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS`
- macOS AUv3: `cmake -S . -B build-macos -G Xcode`