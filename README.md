# tinyplug
*Hypermodern C++ Audio Plug-in Framework*

## Features
Tinyplug is a modern C++ audio plug-in framework that makes it easy to build your plug-in for different platforms and formats.

- Supported platforms & formats:
    | Platform | AAX | AUv2 | AUv3 | CLAP | LV2  | VST3 |
    |---------:|:---:|:----:|:----:|:----:|:----:|:----:|
    | iOS      |     |      |  ✔   |      |      |      |
    | Linux    |     |      |      | TODO | TODO | TODO |
    | macOS    |  ✔  |  ✔   |  ✔   |  ✔   |      |  ✔   |
    | Windows  |  ✔  |      |      |  ✔   |      |  ✔   |
- GPU-backed graphics via Google's Skia Library.
- Supported plug-in types:
    - Stereo in/out effect
    - Optional sidechain
    - Mono in/out effect (TODO)
- Sample-accurate events & automation
    - Tinyplug automatically interleaves host events (including ramps) with calls to your process function for precise automation playback.

## Architecture
Your processor (`Dsp_kernel`) and editor (`Plug_editor`) are fully decoupled. Tinyplug handles communication between these classes and the host via simple message queues. There is no sharing of data between threads. The framework makes the source of truth wherever the plug-in format wants it to be.

### Processor (`DSP_kernel`)
- Receives parameter changes on real-time thread.
- Can send data to the UI by writing to an array of `exports`.

### Editor (`Plug_editor`)
- On draw, receives a view into the current state of the parameters and exports.
- Controls can set parameters by sending `User_actions`.

## Parameters
Your plug-in implements a static interface (`Param_model`) where you enumerate the parameter and export identifiers. You declare your parameter infos as a tree structure. The framework preserves this structure where the format allows (e.g. AUv2 clumps). In practice, parameter values are stored as a flat array and the identifier can be used as an index. There are some restrictions to this approach but they are documented.

Tinyplug makes some decisions for you so your parameters have clear semantics and handling by the host.
- Type-safe parameter semantics via `std::variant`
    - `Bool`: for values that are "true" or "false"
    - `List`: for indices in a list
    - `Int`: for integers
    - `Real`: for continuous values
- Fully enumerated host policies (no flags)
    | Policy         | Automatable? | Visible? | Persistent?|
    |---------------:|:------------:|:--------:|:----------:|
    | `automation`   |      ✅      |    ✅    |     ✅     |
    | `control`      |      ❌      |    ✅    |     ✅     |
    | `state`        |      ❌      |    ❌    |     ✅     |
    | `interface`    |      ❌      |    ❌    |     ❌     |

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
- Mono processing
    - Add CMake option `TINY_SUPPORTS_MONO`?
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
### Todo
- Exports Demo
- Musical Context Demo
- Params Demo
    - Semantics
    - Policy
- Platform Demo
    - Dialogs
    - ...
- Sidechain Demo

## Consider
- How to handle logical vs. real size?
- Optionals in `Musical_context`?
- View notification system? For example, dark mode.
- Use a heuristic for queue sizes.
- Remove `Clap_kernel` and consolidate code.

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