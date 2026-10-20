# Plan: Web format — tinyplug plug-ins in the browser

> Status: **design.** A sixth format beside AAX, AUv2, AUv3, CLAP and VST3: the same `Processor` / `Editor`
> pair, compiled with Emscripten, running in a web page. The processor runs in an AudioWorklet; the editor
> runs on the page's main thread, drawing through Skia on WebGL2. One `make_web_plugin(target)` per plug-in,
> from the same sources and the same tag as the installers.

## Why

Web demos on a product site, built from the shipping C++ so they can't drift from it. The C++ is the source of
truth for every target; design tools (sketch_design) stay design tools.

What the hand-built demos (`vs_comp_web`: VS Comp and OK Channel, 2026-10) taught:

- **The DSP ports cleanly.** A Principal_effect plus spk / vsl compiles to a standalone wasm and runs in an
  AudioWorklet bit-identical to native, at 1-3 % of a core. vsl's SSE path maps onto SIMD128 through
  Emscripten's SSE headers.
- **Everything around the DSP was re-implemented, and drifted.** Each demo has its own C wrapper doing a rough
  copy of the adapter (param glides, prepare cadence, bypass): VS Comp's web bypass fades against an
  undelayed dry. The page maps controls to params by name and patches layouts that predate the plug-in. The UI
  is the design tool's TypeScript renderer, a second implementation of every widget.
- **Platform assumptions surface as build patches.** `platform_defs.hpp` `#error`s and `denormal_guard.hpp`
  calls `_mm_setcsr`; the demo build patches a copy of the headers. A dependency (signalsmith-dsp's
  `perf.h`) reads the x86 FP register under `__SSE__`, which Emscripten defines.

A format fixes all three at the source: the wrapper is tinyplug's, so the adapter, ramping, bypass and state
are the ones every other format uses; the editor is the plug-in's own SUI editor; the platform is a real one.

## Shape

```
tinyplug/
  libs/tiny_core            + TINY_PLATFORM_WASM; denormal guard no-op; web Relay
  libs/tiny_platform        + web_view.cpp, web_context.cpp, web_paths.cpp, web_dialogs.cpp
  wrappers/web/             + make_web_plugin.cmake, source/ (plugin, entry, exports), js/ (host package)
  CMakePresets.json         + "web" (Emscripten toolchain, build-web)
tiny_deps/
  build-skia-wasm.sh        + Skia for wasm32: Ganesh GL on WebGL2, no Metal / Graphite
```

`make_web_plugin(target)` produces, per product:

```
<Base>.web/
  <base>.wasm, <base>.js     the module and Emscripten's loader (threads + audio worklet glue)
  manifest.json              plug_info + the param / meter models: identifiers, ranges, units, choices,
                             defaults, groups; latency; I/O (sidechain, notes)
  fonts/, assets/            whatever the editor embeds that isn't in the module
```

The page side is a small JS package, `wrappers/web/js` (`@tinyplug/web`): it owns the AudioContext, mounts the
editor on a `<canvas>`, wires sources (a loop, a file, the mic) and the sidechain, and exposes a host API:
`load(url)`, `play / stop`, `setParam(id, plain)`, `getState() / setState(bytes)`, `onMeters`, transport
(tempo, playing). It is the DAW; the product site is written against it and knows nothing product-specific.

## Runtime topology

The browser splits the plug-in across two threads that native hosts give us for free: the AudioWorklet
(audio) and the page's main thread (UI, events, timers).

### A. Shared memory (primary)

One module, one `WebAssembly.Memory` shared between the main thread and the AudioWorklet (Emscripten's Wasm
Audio Worklets: `-sAUDIO_WORKLET -sWASM_WORKERS`, plus `-pthread` for `Task_manager`'s lanes and the worker).
The wrapper is then the CLAP wrapper's shape: one `Plugin` object owning the processor, the editor, the
action queue, the meter / block mailboxes, the undo history and the state pair (`connect_in_process`).
The lock-free queues work unchanged, because the atomics are real.

Cost: the page must be **cross-origin isolated** (`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`), which every asset on it (fonts, audio, embeds) has to allow.
Fine on a site we control; it rules out dropping a demo into a third-party page.

### B. Split (fallback)

Two module instances with separate memory: the processor in the worklet, the editor on the main thread, joined
by a `MessagePort`. This is the VST3 / AAX shape (component and controller apart) and the transports already
exist: `state::connect_remote` for state, the action queue → `process::Event`s, meters and blocks as
serialized mailbox frames. More code and a copy per message, but no isolation headers and no threads.

Plan for A; keep B's seams (everything already crosses through queues), and build it only if A fails on a
target browser (see Risks: Safari).

### Processor-only (phase 1)

No editor, no threads: the processor alone in the worklet, the page drawing its own UI against the host API
and the manifest. This is what the current demos are, done properly, and it needs none of the platform work.
It is a forced-headless build (`plans/headless-plugin.md`): the web format is the first real user of that
plan, and the same CMake switch (`TINY_HAS_EDITOR`) decides it.

## The wrapper (`wrappers/web`)

Mostly the CLAP wrapper's logic with a browser for a host:

- **Lifecycle.** `configure(Config)` when the AudioContext's rate is known (and on a rate change: a new context
  means a new configure); `reset(Hard)` on transport jumps and source changes; `process` per 128-frame quantum.
  `Render_mode::Realtime` always (an `OfflineAudioContext` bounce is a later, cheap extra).
- **Params.** The page and the editor both edit through the same path the native wrappers use: page edits are
  host automation (`Set` / `Ramp` into `handle`, sample-accurate at the quantum's offset); editor edits are
  `User_action`s through the action queue, with undo. Host-space values for the page come from the manifest.
- **Latency.** No delay compensation to negotiate: accept every proposal at once (`Reset::Latency` at the next
  block boundary) so the processor contract holds; report it to the page.
- **Bypass.** `tiny_dsp::Host_bypass`, latency-aligned, as in the native wrappers (the demos' bug class).
- **Timeline.** From the page's transport through `Timeline_resolver`, leaving empty what the page doesn't know; tempo-synced
  products need it.
- **I/O.** Main input from the page's source; a second worklet input for the sidechain; notes from Web MIDI or
  an on-screen keyboard (`TINY_HAS_NOTES_IN`), later.
- **State.** `getState / setState` through `State_adapter`, the same bytes the other formats save; the page
  stores them (localStorage / OPFS) and can load factory presets. `Format::Web` in `Edit_context` so editors
  can hide what doesn't apply (licensing, host-only menus).
- **Meters / blocks.** The mailboxes, drained by the editor in its draw; the page gets a copy for its own use.

## The platform (`tiny_platform`, web)

| Piece | Native | Web |
|---|---|---|
| `Platform_view` | NSView / UIView / HWND | a `<canvas>` (Emscripten html5: size, device pixel ratio, resize observer) |
| Input → `User_interaction` / `Event_stream` | mouse, trackpad, touch, keys, wheel | pointer events (mouse + touch through one path), wheel, keys, double-click; pointer capture for drags |
| Draw loop | CVDisplayLink / vsync thread | `requestAnimationFrame` (`emscripten_request_animation_frame_loop`); pauses on hidden tabs, which also pumps `Task_manager::run_main`, so a hidden tab stalls main-thread tasks as an occluded native editor does |
| `Window_context` | Metal / D3D12 / CPU Skia surface | Skia Ganesh on WebGL2 (`GrDirectContexts::MakeGL`, the canvas's default framebuffer); CPU raster + `putImageData` fallback |
| Dark mode | system notification | `matchMedia("(prefers-color-scheme: dark)")` → `Dark_mode_changed` |
| `Relay` | GCD / message window | post to the main runtime thread (`emscripten_async_run_in_main_runtime_thread`), or a flag the rAF loop polls |
| `Platform_paths` | app support dirs | a virtual FS: MEMFS for scratch, OPFS (or IDBFS) mounted for user presets and prefs |
| `Platform_dialogs` | NSOpenPanel etc. | file input / download links; alerts as HTML overlays (async: the dialog API is already callback-based) |
| `Window_token` | parenting | trivially the canvas |

Fonts: no system fonts in wasm. Skia's custom font manager (empty or embedded) and the editors' embedded font
data (`Font_manager`, as Monorobot's are) cover it; anything that asks the system for a family needs a bundled
fallback.

## Skia for wasm (`tiny_deps`)

`build-skia-wasm.sh` beside the mac / iOS / win scripts: emsdk, `gn gen` with `target_cpu = "wasm"` and the
Emscripten toolchain (Skia's own CanvasKit build does this), the same library set as macOS (`libskia`,
`libsvg`, `libskshaper`, `libskparagraph`, `libskunicode_core`, `libskunicode_libgrapheme`, …):

- `skia_use_gl = true`, `skia_use_webgl = true`; Metal, Graphite and Dawn off.
- No system libs (already the convention); `skia_enable_fontmgr_custom_embedded` or `_empty`.
- `-pthread` if topology A, so Skia's objects are built for shared memory (all libs in a threaded module must be).
- `is_official_build`, `-O3` / `-Oz` variants; measure.

Size is the number to watch: CanvasKit (Skia plus JS bindings) is several MB of wasm. Our link only keeps what
SUI calls, so it should be smaller; measure before committing to the editor phases. Brotli on the site.

## Build

- **Preset.** `web` in `CMakePresets.json`: Emscripten's toolchain file, `build-web`, Release. Only the web
  targets (and tiny_core / tinyplug / tiny_platform for wasm) build there.
- **`TINY_PLATFORM_WASM`** in `platform_defs.hpp` (`__EMSCRIPTEN__`), and a no-op `denormal_guard` (wasm has no
  FP control register; denormals are slow on x86 clients, so DSP that can flush should).
- **Flags.** `-msimd128` (and `-msse4.2` for vsl's SSE path until it grows a native wasm SIMD path),
  `-fwasm-exceptions` if anything throws (nlohmann::json), `-sALLOW_MEMORY_GROWTH` off in the worklet (views
  into memory must stay valid) with an initial size from a probe, no name section in Release.
- **Dependencies must be wasm-clean.** spk, vsl, sketch_dsp and vendored code (signalsmith's `perf.h`) get a CI
  job on the `web` preset, so a native-only assumption fails there, not in a product.

## Testing

- **Bit-identity.** A Node runner (no AudioWorklet; the processor-only module) renders a WAV through the web
  build and compares it with a native render of the same processor under the same event schedule, as
  `vs_comp_web`'s `check.mjs` does (bit-identical was achieved there). Lives beside the fake hosts in
  `tests/hosts/`, on the example plug-ins.
- **Wrapper conformance.** The web wrapper against the lifecycle tests the other wrappers pass: configure
  re-entry, `Reset` alternatives, latency acceptance, param authority (a page edit and an editor edit in the
  same quantum), state round-trip.
- **Browser smoke test.** Headless Chrome (`--mute-audio`) over CDP: load, play, drag a control, read a meter,
  screenshot; plus the editor's pixels against the native snapshot tool for the same state.

## Phases

0. **Prerequisites.** `TINY_PLATFORM_WASM`, the denormal guard, the `web` preset; tiny_core / tinyplug build for
   wasm in CI. (Small; unblocks all_plugins' demo build immediately.)
1. **Processor-only web format.** Headless build switch, `wrappers/web` processor side, manifest, the JS host
   package with the worklet, the Node bit-identity test, an example plug-in. Retires the hand wrappers in
   `vs_comp_web`. *(Days.)*
2. **Skia for wasm** in tiny_deps. Measure size and draw cost (a SUI editor at 60 fps in the browser).
   *(Days, mostly build plumbing.)*
3. **Web platform layer.** View, context, input, draw loop, relay, paths, dialogs; an example editor drawing in
   a page with the processor-only module beside it (no shared memory yet: the editor only reads params).
4. **Full format, topology A.** Shared memory, Wasm Audio Worklets, the in-process wrapper with editor, undo,
   state, presets; isolation headers on the demo host. *(Weeks, with 2-3.)*
5. **Ship it.** The `web` preset in the release CI (`all_plugins/plans/ci-build-system.md`), versioned
   artifacts per tag, the product site consuming them; one tag builds installers and demos.

## Risks and open questions

- **Safari.** Wasm Audio Worklets need `SharedArrayBuffer` in the AudioWorklet scope plus wasm threads; verify
  on current Safari (macOS and iOS) early in phase 4, because it decides between topology A and B for the site.
- **Mobile CPU.** Desktop runs these demos at 1-3 % of a core; phones at several times that, and HQ
  oversampling multiplies it. Measure on a mid-range phone in phase 1.
- **Main-thread pump.** `Task_manager`'s main-thread tasks run in the draw; a hidden tab stops rAF and stalls
  them, the same failure as an occluded native editor. Long-running work must keep to background lanes.
- **Exposure.** A wasm build is as reversible as a native one (more, with names). Decide per product whether
  the model ships to the web; keep licensing out of the web build entirely (`Format::Web`).
- **Workers.** Products with a `Plug_worker` need topology A (or B's worker in its own Web Worker).
- **Binary size.** Skia dominates. If the measured size is too big for a product page, the processor-only
  format with a lighter page UI remains a valid end state for some products.
