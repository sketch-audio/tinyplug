# tinyplug
*Minimal, modern C++ audio plug-in framework*

***tinyplug is not version 1.0 yet. I may break it at any time for any reason. I have shipped plug-ins with tinyplug, so backwards compatibility is going to be a priority but not guaranteed and APIs may be changed without notice.***

## Features
Tinyplug is a C++20 audio plug-in framework that makes it easy to write audio plug-ins and build them for different platforms and formats.

- Supported platforms & formats:
    | Platform | AAX | AUv2 | AUv3 | CLAP | VST3 |
    |---------:|:---:|:----:|:----:|:----:|:----:|
    | iOS      |     |      |  ✅  |      |      |
    | macOS    | ✅  |  ✅  |  ✅  |  ✅  |  ✅  |
    | Windows  | ✅  |      |      |  ✅  |  ✅  |
- Supported plug-in types:
    - Stereo in/out effect
    - Optional sidechain input
    - Optionally allow mono in/out
- Sample-accurate events & automation
    - Host events (including ramps) are interleaved with calls to your process function so you have full control over automation playback.
- Thread-safe architecture
    - The framework handles communication between your processor and editor using simple message queues.
- GPU-backed graphics via Google's Skia Library.
    - iOS & macOS: Metal
    - Windows: Direct3D 12 *(NOTE: - Windows uses CPU backend while I can figure out the CPU/GPU synchronization)*

## Architecture
Your processor (`Plug_processor`) and editor (`Plug_editor`) are fully decoupled. Tinyplug handles communication between these classes and the host via simple message queues. There is no sharing of data between threads. The framework makes the source of truth wherever the plug-in format wants it to be.

### Processor (`Plug_processor`)
- Receives parameter changes on real-time thread.
- Can send data to the UI by writing to an array of meter values.

### Editor (`Plug_editor`)
- On draw, receives a read-only copy of the current parameter and meter values.
- Controls can set parameter values by sending `User_actions`.

## Parameters
Your plug-in implements a static interface (`Param_model`) where you enumerate the parameter identifiers. You declare your parameter infos as a tree structure. The framework preserves this structure where the format allows (e.g. AUv2 clumps). In practice, parameter values are stored as a flat array and the identifier can be used as an index. There are some restrictions to this approach but they are documented.

Tinyplug provides a structured interface so your parameters always have well-defined semantics.
- Type-safe parameter semantics via `std::variant`
    - `Bool`: for values that are "true" or "false"
    - `List`: for indices in a list
    - `Int`: for integers
    - `Fixed`: continuous-like but quantized to a fixed step size (ex. 0.5)
    - `Real`: for continuous values, with optional non-linear mapping
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

## Presets
Tinyplug comes with a built-in preset system that allows for tight integration with the plug-in format and DAW.

Presets are json files with an extension of your choosing. They contain the persistable parameter values and the editor state.

- To save a preset, ask for the current preset state (from `State_adapter::Actor`), give it a name, and save it to a file with your custom extension.
- To load a preset from your plug-in's interface, load the json into memory, obtain the knob values (from `State_adapter::Actor`) and emit actions setting the parameter values.
- The host may also load a preset from its own interface, in which case it sets the parameter values. If you need to update your UI in response to a host-loaded preset, consider adding some "extra" state when saving the preset. AUv3 hosts may even save a user preset on their own. If this is the case, the editor state receives an extra `"preset-name"` key with value `std::string`.

Then in your build, let tinyplug know your preset extension and directory. The framework will automatically make your presets available in the native formats and bundle them (when possible) in the correct location so that they may appear in the host interface.
- AAX: `.tfx` placed in your plug-in bundle.
- AUv2: custom
- AUv3: custom
- CLAP: custom
- VST3: `.vstpreset` (will need to be placed by your installer)

## Platform library
- Message dialog
- Confirm dialog
- Text input dialog
- Open url in browser
- Save file picker
- Open file picker
- Dark mode status

## Style
- Use Herb Sutter AAA or "left-to-right" style
    - Most lines of code should begin with `const auto`.
    - Functions are written with trailing return types.
    - See: https://www.youtube.com/watch?v=xnqTKD8uD64
- Use the Stroustrup naming style (modified)
    - Separate words in names with underscores.
    - User types and concepts begin with a single capital letter.
    - Enum cases also begin with a single capital letter (our addition).
    - See: [Cpp Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) (NL.8)
- Opening brace on new line *only* for function definitions.
- Prefix private member variables with an underscore.

## Test Plug-ins
Tinyplug ships with some test plug-ins that can be useful for making sure things are working as advertised.
- Automation Tester (outputs DC)
- Gain Demo
- Latency Demo
- Platform demo

## Build
- Set up dependencies: https://github.com/sketch-audio/tiny_deps
    - Download/build scripts for plug-in SDKs.
    - Download/build scripts for Skia (graphics dependency).
- Be sure to also set `-DTINY_DEPS_PATH=../tiny_deps`
- Unix makefiles: 
    - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug|Release`
    - `cmake --build build`
- iOS AUv3: `cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS`
- macOS AUv3: `cmake -S . -B build-macos -G Xcode`

## Roadmap
- **Additonal processor state** (loop content, etc.)
- **MIDI I/O**
- **Synth plug-ins**
- **Block metering**
- Basic control library
- Multitouch on Windows
- Software (CPU) graphics backend for macOS
- Figure out GPU synchronization situation on Windows
- More demo plug-ins
    - Meters
    - Musical_context
    - Sidechain
- Linux (CLAP & VST3)
- LV2 plug-ins