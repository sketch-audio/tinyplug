# tinyplug
*Hypermodern C++ Audio Plug-in Framework*

## Features
- Write your plug-in once, run it everywhere.
    - Supported platforms: macOS and Windows.
    - GPU-backed graphics via Google's Skia Library.
        - macOS: Metal
        - Windows: Direct3D 12
    - Supported plug-in formats: AAX, AUv2, CLAP, VST3.
    - Supported plug-in types:
        - Stereo in/out effect
        - Optional sidechain
        - Mono in/out effect (TODO)
- Sample-accurate events & automation
    - Tinyplug automatically interleaves host events (including ramps) with calls to your process function for precise automation playback.

## Architecture
Your processor (`Dsp_kernel`) and editor (`Custom_view`) are fully decoupled. Tinyplug handles communication between these classes and the host via simple message queues. There is no sharing of data between threads. The framework makes the source of truth wherever the plug-in format wants it to be.

### Processor (`DSP_kernel`)
- Receives parameter changes on real-time thread.
- Can send data to the UI by writing to an array of `exports`.

### Editor (`Custom_view`)
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
- Add AUv3 format for iOS and macOS
- Multitouch
    - Both iOS and Windows!
- Non-parameter persistence
    - E.g. last loaded preset name, other UI-only data
- Mono processing
    - Add CMake option `TINY_SUPPORTS_MONO`?
- Basic control library
- Presets
- Preferences
    - E.g. light mode/dark mode
    - Scopes: manufacturer, plug-in
- System dialogs
    - File load & save
- Breakout dependencies project
- Update CMake to build multiple user plug-ins
- MIDI events
- Synth support
- Add Linux support for CLAP, VST3
- Add LV2 format for Linux

## Demo Plug-in (TODO)
The demo plug-in should demonstrate & test all stated features.
- Sidechain input
- Musical context
- Value semantics
- Host policies
- Export types
- Latency changes

## Automation Test Plug-in (TODO)
The automation test plug-in should make it easy to demonstrate and test automation playback behavior.
- Output automation data as DC
- Log all automation events received (idea)

## Consider
- How to handle logical vs. real size?
- Optionals in `Musical_context`?
- View notification system? For example, dark mode.
- Use a heuristic for queue sizes.
- Remove `Clap_kernel` and consolidate code.

# CMake Refactorings
- Refactor to allow multiple user plugins
    - Format builds -> CMake functions
- Set up dependencies project and move the window contexts there (tiny_shared/skia)
- Idea: tiny_aax, etc. -> formats, tiny_shared -> shared, tiny_user -> user?
- Idea: move platform into tinyplug proper?
- No globbing

//

tinyplug
- formats/
    - aax/
        - cmake/
        - source/
        - CMakeLists.txt
    - auv2/
    - auv3/
    - clap/
    - lv2/
    - vst3/
- shared/
    - tinyplug/
        - impl/
            - ios_...
            - lin_...
            - mac_...
            - win_...
        - tiny_....h
- plugins/
    - automation_tester/
        - source/
            - custom_view.h/.cpp
            - dsp_kernel.h/.cpp
            - param_model.h
        - CMake_lists.txt
    - gain_demo/
        - ...
    - latency_demo/
        - ...
    - etc.

tinydeps
- skia proper
- skia window contexts