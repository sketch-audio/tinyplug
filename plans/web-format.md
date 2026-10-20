# Plan: Web format — tinyplug plug-ins in the browser

> Status: **phases 0-4 landed on `web-format` (2026-10-20)**: every demo runs in the browser with its own editor.
> Phase 5 (Safari, mobile, polish) and 6 (GPU, size, CI) remain. Direction agreed 2026-10-20. A sixth format beside AAX, AUv2, AUv3, CLAP and VST3: the
> same `Processor` / `Editor` pair, compiled with Emscripten, running in a web page. **Two modules, AAX-shaped**:
> the processor in an AudioWorklet, everything else (param authority, editor, undo, state, worker) in a
> controller module on the page's main thread, joined by rings in shared memory. The editor draws through Skia,
> CPU raster first. One `make_web_plugin(target)` per plug-in, from the same sources and the same tag as the
> installers.

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

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Topology | **Two modules** (processor / controller), not one shared module | Keeps the decoupled model; no threads in the worklet; phase 1 ships unchanged into the full format; audio can start before the editor downloads |
| Cross-origin isolation | **Required for v1** | The rings and the controller's pthreads both need `SharedArrayBuffer`. The non-isolated fallback is deferred (see Topology) |
| Graphics | **Skia CPU raster first**; GPU later as Graphite on WebGPU | Smallest Skia build, no GL in the loop, matches the CPU backends kept on Windows and iOS, and makes pixel tests against a native CPU render feasible |
| Page package | **Plain ES modules**, no build step, no runtime npm dependencies | It is the DAW; it should be readable and vendorable by the product site |
| Toolchain | **One pinned emsdk, owned by tiny_deps**, for Skia and tinyplug alike | Every object in a module must come from one Emscripten (libc++ headers, features); see Skia for wasm |

Open:

- **emsdk version.** Skia m139 pins 3.1.44 (2023); Homebrew has 6.0.12. Pick the newest that builds Skia m139
  cleanly; try 6.0.x first, fall back towards 4.0.x.
- **Browser test driver.** A dev-only puppeteer dependency, or a small raw-CDP client (Node has `WebSocket`
  built in; ~150 lines, no dependencies). Leaning raw CDP.
- **Exposure.** Name section stripped in Release, and `Format::Web` as the switch that keeps licensing out.
- **Scheduled page automation.** v1 lands page `setParam` at the next quantum boundary; an API that maps
  `AudioContext` time to a frame offset can follow.

## Shape

```
tinyplug/
  libs/tiny_core            + TINY_PLATFORM_WASM; denormal guard no-op; web Relay;
                              Byte_ring / Block_store promoted from wrappers/aax (the wire)
  libs/tiny_platform        + web_view.cpp, web_context.cpp (CPU raster), web_paths.cpp, web_dialogs.cpp
  wrappers/web/             + make_web_plugin.cmake
                              source/processor/  (worklet module: exports, the processor half)
                              source/controller/ (main-thread module: authority, editor, view)
                              js/                (@tinyplug/web: worklet processor, host API)
  tests/hosts/web_host.mjs  + Node conformance host
  tools/web_serve.mjs       + static server with COOP / COEP, one host page per demo
  CMakePresets.json         + "web" (the pinned emsdk's toolchain, build-web)
tiny_deps/
  build-skia-web.sh         + Skia for wasm32, CPU raster, -pthread, into build/web/lib
  cmake/setup_skia.cmake    + an EMSCRIPTEN branch
```

`make_web_plugin(target)` produces, per product:

```
<Base>.web/
  <base>_processor.wasm      the worklet module: standalone, no Emscripten JS, fixed shared memory
  <base>.wasm, <base>.js     the controller module and Emscripten's loader (pthreads, html5)
  manifest.json              plug_info + the models: identifiers, ranges, units, choices, defaults, groups;
                             latency; I/O (sidechain, notes). Exported by the module, written at build time
  assets/                    whatever the editor embeds that isn't in the module
```

The page side is `wrappers/web/js` (`@tinyplug/web`): it owns the AudioContext, mounts the editor on a
`<canvas>`, wires sources (a loop, a file, the mic) and the sidechain, and exposes a host API: `load(url)`,
`play / stop`, `setParam(id, plain)`, `getState() / setState(bytes)`, `onMeters`, transport (tempo, playing). It
is the DAW; the product site is written against it and knows nothing product-specific.

## Topology

```
AudioWorklet thread                       Main thread
┌──────────────────────────────┐          ┌───────────────────────────────────────┐
│ processor module             │          │ controller module                     │
│  processor, Host_bypass,     │  rings   │  param authority (the "host"),        │
│  Timeline_resolver, Note_io, │ ◄──────► │  editor + view + undo, Editor_link,   │
│  State_processor, meter /    │          │  worker, Task_manager, platform layer │
│  block publishers            │          │  the page host API calls in here      │
│  no threads, no Skia, no JS  │          │  pthreads, Skia                       │
└──────────────────────────────┘          └───────────────────────────────────────┘
```

This is the AAX split. The processor module is AAX's algorithm: it owns the DSP and has no route back to the
controller except its rings. The controller is AAX's data model and VST3's controller in one, and also the DAW:
page edits, editor gestures and undo all end at one param authority, with no host round-trip.

**The wire.** AAX's `Byte_ring` and `Block_store` are standard-library only and addressed by byte offset into
memory the producer owns; Direct Data is "the remote side memcpys a byte range out of my private data". In the
browser each module keeps its own rings in its own memory, and C++ on either side only ever touches its own.
Nothing is posted on the audio path, and the worklet never allocates a JS object per quantum. Both types move to
tiny_core with their tests (`aax_transport_test`); AAX keeps using them.

**The pump.** A plain-JS dedicated Web Worker (not an Emscripten pthread) holds both modules' shared memories
and moves frames between the processor's rings and their controller-side twins, waiting on `Atomics.wait` with
a short timeout. It is VST3's `Outbound_message_shuttle`, for the same reason: something has to carry bytes
between two sides that share no memory. It exists because neither obvious owner works:

- **Not the main thread.** `requestAnimationFrame` stops in a hidden tab and main-thread timers are throttled
  there, but processor ↔ worker traffic, state snapshots and latency proposals must flow with no editor at all,
  as they do natively.
- **Not the controller's pthreads.** They share the controller's memory, not the processor's; only a JS scope
  holding the processor's `WebAssembly.Memory` can reach its rings.

With the pump, every controller thread reads its own memory natively: the main thread for params and the
editor, `Worker_runner` for its queues. The cost is a JS mirror of the ring header protocol (push, pop, frame
copy), about a hundred lines, tested against the C++ under Node.

**One producer per ring.** Each channel has its own ring and exactly one writing thread, as the native queues
do (`spsc` from the processor and the editor; `Worker_replies` from the worker thread). Controller → processor
traffic comes from several threads, so it is several rings, never one shared one.

Channels, each sized and framed at compile time from the models (`configure_models` already gives every count):

| Direction | Channel | Native precedent |
|---|---|---|
| controller → processor | param events (plain `Set` / `Ramp`, at an offset) | AAX coefficient segments, VST3 param queues |
| controller → processor | state edits | `state::connect_remote`, VST3 `tiny/state/edit` |
| controller → processor | notes from the editor, the page, Web MIDI | VST3 `tiny/notes` |
| controller → processor | worker replies to the processor | VST3 `tiny/worker/to_processor` |
| controller → processor | transport (tempo, playing, position), bypass, reset requests, accepted latency | AAX runtime packet |
| processor → controller | meters | AAX `Ring_meter` |
| processor → controller | blocks | AAX `Block_store`, VST3 `tiny/blocks` |
| processor → controller | state snapshots | VST3 `tiny/state/snapshot` |
| processor → controller | notes out (to the page, Web MIDI out) | — |
| processor → controller | worker messages from the processor | VST3 `tiny/worker/from_processor` |
| processor → controller | latency proposals | AAX `Ring_latency` |

As on AAX, the producer never overwrites unread data (a full push drops), so the consumer reads without a
seqlock retry, and nothing may assume the consumer's rate. Ring sizes come from the models (the work channels
from `User_work::inbound_capacity` / `outbound_capacity`), and every alternative is trivially copyable, so
frames are fixed-size.

**Memory.** The processor module's memory is shared (`-sSHARED_MEMORY`, so `SharedArrayBuffer`-backed) and of a
fixed size chosen from a probe: growing would leave the controller's views at the old length. The controller
module is an ordinary Emscripten pthreads module with its own memory.

**One compile flavor.** Wasm objects built with atomics can't link into a module without shared memory, so both
modules build with `-pthread` (atomics + bulk memory); the processor module simply starts no threads. That is
also why the non-isolated fallback is a second flavor rather than a switch.

**Non-isolated fallback (deferred).** The same two modules without `SharedArrayBuffer`: the wire over the
worklet's `MessagePort` (a frame per message, posted at most per quantum and coalesced for meters), and a
`Task_manager` whose lanes run cooperatively on the main thread instead of on pthreads. Needed only to embed a
demo in a page we don't control. The wire's interface is the seam; nothing above it changes. **A plug-in with a
worker always requires isolation**: `Worker_runner` has no cooperative mode, and a worker on main-thread time
slices would stall in a hidden tab.

**Rejected: one shared module.** One `WebAssembly.Memory` shared by the main thread and the worklet (Emscripten's
Wasm Audio Worklets, `-sAUDIO_WORKLET -sWASM_WORKERS`) would make the wrapper the CLAP wrapper's shape. It puts
threads and Emscripten's worklet glue in the audio thread (the Safari risk), couples the editor's download to
the first sound, and gives up the separation the format was designed for.

## The processor module

A standalone wasm (no Emscripten JS runtime), instantiated by our own `AudioWorkletProcessor` from a compiled
`WebAssembly.Module` the page posts to it. The worklet supplies the few imports (memory, abort, a logging
stub) and calls the exports:

- `configure(sr, params*)` once the AudioContext's rate is known, from the worklet's init message, not from
  `process` (it allocates; nothing is rendering yet). A rate change is a new context, so a new configure.
- `process(frames)` per quantum: copy the worklet's planar inputs into module buffers, drain the inbound rings
  into events at their offsets, render through the same event loop as the native wrappers, push meters /
  blocks / snapshots / notes / proposals, copy outputs back. 128 frames today; Chrome's configurable render
  size means sizing for a maximum and accepting fewer.
- Lifecycle as the native wrappers: `reset(Hard)` on transport jumps and source changes, `Render_mode::Realtime`
  always (an `OfflineAudioContext` bounce is a later, cheap extra).
- **Latency.** No delay compensation to negotiate: a proposal is accepted at once and arrives back as
  `Reset::Latency` at the next block boundary, so the processor contract holds; the controller reports it to
  the page. `Host_bypass::set_latency` in lockstep.
- **Timeline.** Transport messages from the page set tempo, playing and a position anchored to a context
  frame; the module derives each quantum's position from its own frame counter and runs `Timeline_resolver`,
  leaving empty what the page doesn't say.
- **I/O.** Main input from the page's source; a second worklet input for the sidechain; an instrument has
  no input bus.

The plug-in's `<P>_lib` compiles unchanged: a static archive only contributes the objects a module references,
so the processor module never links `editor.o` or Skia, and no headless switch is needed.

## The controller module

The CLAP wrapper's logic with the processor across the wire:

- **Params.** The authority holds every value. Page edits are host automation (`Set` / `Ramp` to the
  processor at the next quantum); editor edits are `User_action`s through the action queue, with undo, exactly
  as native. Host-space values for the page come from the manifest.
- **State.** `getState / setState` through `State_adapter`, the same bytes the other formats save; the page
  stores them (localStorage / OPFS) and can load factory presets. A page `setState` is a host load: one undo
  step, `Editor::notify(Host_preset_loaded)`. `Format::Web` in `Edit_context` lets editors hide what doesn't
  apply (licensing, host-only menus).
- **Meters / blocks.** Drained from the rings into the mailboxes the editor reads in its draw; the page gets a
  copy for its own use (`onMeters`).
- **Worker.** On the controller side, as VST3's: editor ↔ worker direct (in-process queues), processor ↔
  worker across the wire through the pump, so it runs with the tab hidden and the editor closed. Its replies to
  the processor go on a ring only the worker thread writes. worker_demo is the acceptance test.
- **Threads.** `Task_manager` lanes and `Worker_runner` on Emscripten pthreads, from a prewarmed pool
  (`-sPTHREAD_POOL_SIZE`): thread start in a browser is asynchronous, so a lane created after load could
  otherwise stall until the main thread yields.

## The platform (`tiny_platform`, web)

| Piece | Native | Web |
|---|---|---|
| `Platform_view` | NSView / UIView / HWND | a `<canvas>` (Emscripten html5: size, device pixel ratio, resize observer) |
| Input → `User_interaction` / `Event_stream` | mouse, trackpad, touch, keys, wheel | pointer events (mouse + touch through one path), wheel, keys, double-click; pointer capture for drags |
| Draw loop | CVDisplayLink / vsync thread | `requestAnimationFrame` (`emscripten_request_animation_frame_loop`); pauses on hidden tabs, which also pumps `Task_manager::run_main`, so a hidden tab stalls main-thread tasks as an occluded native editor does |
| `Window_context` | Metal / D3D12 / CPU Skia surface | Skia CPU raster into a buffer, `putImageData` on a 2D canvas; later Graphite on WebGPU |
| Dark mode | system notification | `matchMedia("(prefers-color-scheme: dark)")` → `Dark_mode_changed` |
| `Relay` | GCD / message window | post to the main runtime thread (`emscripten_async_run_in_main_runtime_thread`), or a flag the rAF loop polls |
| `Platform_paths` | app support dirs | a virtual FS: MEMFS for scratch, IDBFS (or OPFS) mounted for user presets and prefs |
| `Platform_dialogs` | NSOpenPanel etc. | file input / download links; alerts as HTML overlays (async: the dialog API is already callback-based) |
| `Window_token` | parenting | trivially the canvas |

Fonts: no system fonts in wasm. The demos draw with `tiny_ui`'s pixel font and need no font manager. Products
use Skia's custom font manager (empty or embedded) and the editors' embedded font data (`Font_manager`, as
Monorobot's are); anything that asks the system for a family needs a bundled fallback.

**GPU, later.** Skia's newer backend, Graphite, runs on WebGPU through Dawn (`skia_use_dawn`,
`skia_use_webgpu`; CanvasKit's `ENABLE_WEBGPU`), which Chrome and current Safari ship. That is the GPU step,
not Ganesh on WebGL2, which stays only as a compatibility path if a target browser lacks WebGPU. Out of scope
for this push: CPU raster first, measure, then decide.

## Skia for wasm (`tiny_deps`)

Confirmed in the checkout (`build/src/skia`, `chrome/m139`): `gn/toolchain/BUILD.gn` defines a `wasm` toolchain
when `target_cpu = "wasm"`, calling `$skia_emsdk_dir/upstream/emscripten/{emcc,em++,emar}`; `skia_emsdk_dir`
defaults to `third_party/externals/emsdk`, which `git-sync-deps` fetched and `bin/activate-emsdk` activated at
**3.1.44**. CanvasKit's `modules/canvaskit/compile.sh` is the reference set of gn args.

`build-skia-web.sh`, in the shape of `build-skia-mac.sh`:

1. `setup_depot_tools`, `sync_deps` as today.
2. Install and activate **the pinned emsdk** (the Decisions table) into `build/tmp/emsdk`, through the emsdk
   checkout already in externals. Never `bin/activate-emsdk`, which would select 3.1.44.
3. `gn gen` with `target_cpu = "wasm"`, `skia_emsdk_dir` pointing at it, `is_official_build = true`,
   `werror = false`; Ganesh, Graphite, Dawn, WebGL, Metal and Vulkan off (CPU raster);
   `skia_use_freetype = true` with `skia_enable_fontmgr_custom_empty` (the embedded font manager comes when a
   product needs it); no system libs (already the convention); `extra_cflags = ["-pthread"]`, because every
   object in a threaded module must be built for shared memory.
4. `ninja`, then move the same library set as macOS (`libskia`, `libskottie`, `libskshaper`, `libsksg`,
   `libskparagraph`, `libskunicode_core`, `libskunicode_libgrapheme`, `libsvg`, `libpathops`) into
   `build/web/lib`. Drop any that fail to build for wasm rather than patching Skia; nothing in the demos needs
   more than `libskia`.

`setup_skia.cmake` gains an `EMSCRIPTEN` branch pointing at `build/web/lib`. tinyplug's `web` preset uses the
same emsdk's toolchain file (`build/tmp/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`, via
`TINY_DEPS_PATH`), so Skia and tinyplug can't drift apart.

Size is the number to watch: CanvasKit is several MB of wasm. Our link only keeps what SUI calls, and CPU raster
drops the GPU code entirely; measure the gain demo's controller module before the editor phases. Brotli on the
site.

## Build

- **Preset.** `web` in `CMakePresets.json`: the pinned emsdk's toolchain, `build-web`, Release. Only the web
  targets (and tiny_core / tinyplug / tiny_platform for wasm) build there; plus `web-tests` for the test suite
  under Node.
- **`TINY_PLATFORM_WASM`** in `platform_defs.hpp` (`__EMSCRIPTEN__`), and a no-op `denormal_guard` (wasm has no
  FP control register; denormals are slow on x86 clients, so DSP that can flush should).
- **Flags.** `-pthread` everywhere (see Topology); `-msimd128` (and `-msse4.2` for vsl's SSE path until it
  grows a native wasm SIMD path); `-fwasm-exceptions` if anything throws (nlohmann::json);
  `-ffp-contract=off` on both web and native builds being compared for bit-identity; no name section in
  Release.
- **Dependencies must be wasm-clean.** spk, vsl, sketch_dsp and vendored code (signalsmith's `perf.h`) get a CI
  job on the `web` preset, so a native-only assumption fails there, not in a product.

## Verification

Every layer has a cheap automated check before a browser is involved.

1. **The existing tests under Node.** tiny_core and `tests/` built for wasm with
   `CMAKE_CROSSCOMPILING_EMULATOR=node`, run by `ctest --preset web-tests`: lock-free queues, state, change sets,
   tasks (pthreads in Node worker threads) and the wire, on the target ABI.
2. **`tests/hosts/web_host.mjs`**, a Node conformance host in the style of `clap_host`: the processor module in a
   worker thread, the controller module (no view) in the main thread, over the real shared-memory rings. One
   scenario per demo:

   | Demo | Proves |
   |---|---|
   | gain, meter | params (a page edit and an editor edit in one quantum), meters, bypass |
   | automation | sample-accurate `Set` / `Ramp`, the block log |
   | block | blocks, snapshot and stream policies |
   | latency | proposal → immediate accept → `Reset::Latency`, bypass PDC |
   | state | state edit / snapshot round trip, a page `setState` as one undo step |
   | step_sequencer | timeline from the page's transport, notes out |
   | sine_synth | notes in, an instrument with no input bus, MPE |
   | worker | processor ↔ worker across the wire, editor ↔ worker direct; Node has no rAF, so this also proves the traffic needs no drawing |
   | platform | paths, dialogs, dark mode (browser only) |

   Plus the lifecycle the other wrappers pass: configure re-entry, every `Reset` alternative, state round trip.
3. **Cross-format equivalence.** The same event schedule rendered through the CLAP build (clap_host) and the web
   build, compared. Exact where it can be, with `-ffp-contract=off` on both sides (gain, latency, automation).
   Anything calling `sin` / `exp` (sine_synth) differs by libm (musl in wasm, Apple's natively) and gets a
   threshold around −120 dB. "Bit-identical" is claimed only where it holds.
4. **Browser smoke tests.** Headless Chrome (`--autoplay-policy=no-user-gesture-required --mute-audio`) over
   CDP, per demo: load, play, drag a control, read a meter back through the page API, screenshot; the
   screenshot against a native CPU-raster render of the same editor and state. Plus a hidden-tab check (emulated
   `visibilityState = hidden`, not a frozen page): worker_demo's round trip and the page's `onMeters` keep
   flowing while the editor stops drawing.
5. **By hand.** `tools/web_serve.mjs` (COOP / COEP headers) and a host page per demo; Chrome and Safari, macOS
   and iOS. Safari is the one that can't be automated here.

## Phases

0. **Prerequisites, in parallel.**
   - tiny_deps: the emsdk pin, `build-skia-web.sh`, `setup_skia.cmake`; a size measurement of a minimal
     Skia link. First, because it is long-running and its numbers can change the editor phases.
   - tinyplug: `TINY_PLATFORM_WASM`, the denormal guard, a web `Relay`, the `web` / `web-tests` presets;
     tiny_core and `tests/` passing under Node.
1. **The wire.** `Byte_ring` / `Block_store` into tiny_core, the channel table, the JS pump and its ring
   mirror; tests natively and under Node, the JS mirror against the C++.
2. **The processor module** and the worklet glue. `web_host` runs gain_demo end to end in Node, then every
   demo's processor half; the manifest; the bare page API. Retires the hand wrappers in `vs_comp_web`.
   *(Days.)*
3. **The controller module, no view.** Param authority, `Editor_link` over `connect_remote`, undo, the worker
   through the pump, `Task_manager` on pthreads; conformance for state and worker_demo.
4. **The web platform layer**, CPU raster: view, input, draw loop, relay, paths, dialogs, dark mode. gain_demo's
   editor in Chrome.
5. **Every demo in the browser**, the CDP smoke tests, a by-hand Safari pass. *(Weeks, with 3-4.)*
6. **Later.** Graphite on WebGPU; size work; the non-isolated fallback; the `web` preset in the release CI
   (`all_plugins/plans/ci-build-system.md`), versioned artifacts per tag, the product site consuming them.

## Risks and open questions

- **Seqlocks need atomic data in wasm (found in phase 2).** Binaryen strips `atomic.fence` at -O2 and above, since
  the wasm memory model gives fences no meaning for plain accesses, and V8 compiles atomics on ARM as `ldar` /
  `stlr`, which let a plain copy drift past them. `wire::Block_store` therefore copies its slots as relaxed atomic
  words (plain moves natively) and `wire.js` as per-word `Atomics`; before that, the Release wasm build accepted
  torn frames under a fast publisher. Rings are unaffected: message passing over SC atomics is data-race-free.
  `Block_store::publish_with`'s in-place fill stays plain, so it is for native readers only.

- **Safari.** `SharedArrayBuffer` needs isolation everywhere, and the controller needs wasm threads on the main
  thread; check current Safari (macOS and iOS) in phase 2 with the processor module alone, before the
  controller depends on it. The two-module shape keeps threads out of the worklet, which was the original risk.
- **Toolchain drift.** Homebrew's Emscripten moves on `brew upgrade`; only the tiny_deps pin builds. Skia m139
  was tested against 3.1.44, so a newer emsdk may need warnings off or a patch; the script records what it took.
- **Blocking on the main thread.** `Task_manager::shutdown` waits for running tasks; Emscripten implements that
  wait by spinning, and it deadlocks if the task proxies anything to the main thread. Tasks must never call
  back into the main thread: already the rule natively, but here breaking it hangs the tab.
- **Mobile CPU.** Desktop runs these demos at 1-3 % of a core; phones at several times that, and HQ
  oversampling multiplies it. Measure on a mid-range phone in phase 2.
- **Hidden tabs.** A hidden tab stops rAF, which stalls `Task_manager`'s main-thread tasks and the editor's
  mailbox reads, as an occluded native editor does. The wire itself keeps moving (the pump is a dedicated
  worker), so the processor, the worker and the page's `onMeters` are unaffected. Long-running work keeps to
  background lanes. Verify that browsers don't throttle a dedicated worker's `Atomics.wait` timeout in a
  background tab; if one does, the pump falls back to blocking on a futex the worklet signals each quantum.
- **Exposure.** A wasm build is as reversible as a native one (more, with names). Decide per product whether
  the model ships to the web; keep licensing out of the web build entirely (`Format::Web`).
- **Binary size.** Skia dominates the controller. If the measured size is too big for a product page, the
  processor module with a lighter page UI against the manifest remains a valid end state for some products,
  and needs no change to the format.
