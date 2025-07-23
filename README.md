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
    - Enumerate parameter identifiers.
    - Declare parameters as a tree of `Param_spec` structs.
- Type-safe parameter value semantics via `std::variant`
    - Bool, List, Float, Int
- Sample-accurate events & automation
    - Tinyplug automatically interleaves host events (including ramps) with calls to your process function for precise automation playback.

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
- Add AUv3 format for iOS and macOS
- Add Linux support for CLAP, VST3
- Add LV2 format for Linux
- Breakout dependencies project
- MIDI
- Parameter tree validation
    - Identifiers should be unique, all identifiers should appear in tree.
- Parameter tree versioning
- Synth support