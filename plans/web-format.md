# Plan: Web format — tinyplug plug-ins in the browser

> Status: **landed on main (2026-10-22).** Every demo runs in the browser with its own editor, in two variants:
> `isolated` (COOP / COEP) and `nonisolated` (any page). Checked by hand in Chrome and Safari on macOS. What
> remains is the by-hand checks and test gaps under "Next", and "Later" (GPU, CI).
>
> A sixth format beside AAX, AUv2, AUv3, CLAP and VST3: the same `Processor` / `Editor` pair, compiled with
> Emscripten. **Two modules, AAX-shaped**: the processor in an AudioWorklet, everything else (param authority,
> editor, undo, state, worker) in a controller module on the page's main thread, joined by rings. The editor
> draws through Skia, CPU raster. One `make_web_plugin(target)` per plug-in, from the same sources and the same
> tag as the installers.

## Why

Web demos on a product site, built from the shipping C++ so they can't drift from it. The hand-built demos
(`vs_comp_web`: VS Comp and OK Channel, 2026-10) showed the DSP ports cleanly (bit-identical, 1-3 % of a core)
but everything around it was re-implemented and drifted: a rough copy of the adapter per demo, controls mapped by
name, a second implementation of every widget, and platform assumptions patched into copies of the headers. A
format fixes all three at the source.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Topology | **Two modules** (processor / controller), not one shared module | Keeps the decoupled model; no threads in the worklet; audio can start before the editor downloads |
| Cross-origin isolation | **Two variants**, `TINY_WEB_VARIANT` = `isolated` (default) or `nonisolated` | Isolated: the rings and the controller's pthreads need `SharedArrayBuffer`, so the page sends COOP / COEP and every embed beside it must opt in. Nonisolated: any page, no threads, no worker (see Variants) |
| Graphics | **Skia CPU raster**; GPU later as Graphite on WebGPU | Smallest Skia build, no GL in the loop, matches the CPU backends on Windows and iOS, allows pixel tests against a native CPU render |
| Page package | **Plain ES modules**, no build step, no runtime npm dependencies | It is the DAW; it should be readable and vendorable by the product site |
| Toolchain | **emsdk 6.0.12, pinned by tiny_deps** (`build-skia-web.sh`), for Skia and tinyplug alike | Every object in a module must come from one Emscripten. Never Skia's `bin/activate-emsdk`, which selects 3.1.44 |
| Browser test driver | **Raw CDP** over Node's built-in `WebSocket` | No dependencies |
| Exposure | Name section stripped in Release; `Format::Web` in `Edit_context` keeps licensing out | A wasm build is as reversible as a native one |

**Rejected: one shared module.** Emscripten's Wasm Audio Worklets (`-sAUDIO_WORKLET -sWASM_WORKERS`) would put
threads and Emscripten's glue in the audio thread, couple the editor's download to the first sound, and give up
the separation the format was designed for.

## Shape

```
tinyplug/
  libs/tiny_core            TINY_PLATFORM_WASM, TINY_PLATFORM_THREADS; denormal guard no-op; web Relay;
                            wire.hpp (Byte_ring / Block_store, promoted from wrappers/aax)
  libs/tiny_platform        web_view.cpp, web_context.cpp (CPU raster), web_paths.cpp, web_dialogs.cpp
  wrappers/web/             make_web_plugin.cmake
                            source/processor/  (worklet module)
                            source/controller/ (main-thread module: authority, editor, view)
                            js/                (worklet, host API, pump, link, wire mirror)
  tests/hosts/web_host.mjs, web_split_host.mjs; tests/web/
  tools/web/serve.mjs       static server with COOP / COEP (or --no-isolation), host pages per demo
  CMakePresets.json         web, web-nonisolated, web-tests
tiny_deps/
  build-skia-web.sh [nonisolated]   Skia for wasm32, CPU raster, into build/web/lib
```

`make_web_plugin(target)` produces a self-contained, deployable `<Base>.web/`: `<base>_processor.wasm` (standalone,
no Emscripten JS), the controller module and its loader, `manifest.json` (plug_info and the models, plus
`"variant"`), the JS and an `index.html`.

The page side is the host API in `wrappers/web/js`: it owns the AudioContext, mounts the editor on a `<canvas>`,
wires sources and the sidechain, and exposes `load(url)`, `play / stop`, `setParam(id, plain)`,
`getState() / setState(bytes)`, `onMeters`, `onMidi` and transport. It is the DAW; the product site is written
against it and knows nothing product-specific. Where sessions are stored, audio sources and routing, Web MIDI
devices and which presets a product offers are the page's.

## Topology

```
AudioWorklet thread                       Main thread
┌──────────────────────────────┐          ┌───────────────────────────────────────┐
│ processor module             │          │ controller module                     │
│  processor, Host_bypass,     │  rings   │  param authority (the "host"),        │
│  Timeline_resolver, Note_io, │ ◄──────► │  editor + view + undo, Editor_link,   │
│  State_processor, meter /    │          │  worker, Task_manager, platform layer │
│  block publishers            │          │  the page host API calls in here      │
│  no threads, no Skia, no JS  │          │  pthreads (isolated), Skia            │
└──────────────────────────────┘          └───────────────────────────────────────┘
```

The processor module is AAX's algorithm: it owns the DSP and has no route back except its rings. The controller is
AAX's data model and VST3's controller in one, and also the DAW: page edits, editor gestures and undo all end at
one param authority, with no host round-trip.

**The wire.** Each module keeps its own rings in its own memory, and C++ on either side only touches its own.
Nothing is posted on the audio path, and the worklet never allocates a JS object per quantum. As on AAX, a full
push drops, so the consumer reads without a seqlock retry and nothing may assume its rate. Ring sizes come from the
models, and every alternative is trivially copyable.

**The pump** (isolated). A plain-JS dedicated Web Worker holds both shared memories and moves frames between the
processor's rings and their controller-side twins (VST3's worker relay, for the same reason). Not
the main thread, because rAF stops and timers throttle in a hidden tab; not a controller pthread, because only a
JS scope holding the processor's `WebAssembly.Memory` can reach its rings. Processor messages are routed by kind
so each controller ring has one reader.

**One producer per ring.** Controller → processor traffic comes from several threads, so it is several rings.

| Direction | Channel | Native precedent |
|---|---|---|
| controller → processor | param events (plain `Set` / `Ramp`, at an offset) | AAX coefficient segments, VST3 param queues |
| controller → processor | state edits; host (session) loads, pumped before edits | `state::connect_remote`, VST3 `tiny/state/edit` |
| controller → processor | notes from the editor, the page, Web MIDI | VST3 `tiny/notes` |
| controller → processor | worker replies to the processor | VST3 `tiny/worker/to_processor` |
| controller → processor | transport, bypass, reset requests, accepted latency | AAX runtime packet |
| processor → controller | meters, blocks, state snapshots | AAX `Ring_meter` / `Block_store`, VST3 messages |
| processor → controller | notes out, worker messages, latency proposals | VST3 `tiny/worker/from_processor`, AAX `Ring_latency` |

**Memory.** The processor module's memory is shared and of a fixed size: growing would leave the other side's
views at the old length.

**One compile flavor per tree.** Wasm objects built with atomics can't link into a module without shared memory,
so the isolated tree builds everything with `-pthread` and the nonisolated tree builds nothing with it. Hence two
trees (and two Skia builds), not a switch.

**Variants.**
- The wire is the seam; nothing above it changes. In `nonisolated`, [link.js](../wrappers/web/js/link.js) splits
  the pump in two halves over the worklet's `MessagePort`. The worklet posts every 4 quanta; the page sends each
  frame, on a 10 ms timer, and whenever the worklet posts. A full ring parks posted bytes, then drops whole posts
  past 4x its capacity.
- `TINY_PLATFORM_THREADS` is 0: `Task_manager` runs its background and serial lanes on main from `run_main`, 4 ms
  at a time; the logger drains on the caller; `Relay` clears directly.
- **A plug-in with a worker needs `isolated`**: `Worker_runner` has no cooperative mode. `make_web_plugin` stops
  the configure with an error.
- `host.js` picks the pump or the link from the manifest's `"variant"`, and refuses an isolated build on a page
  that isn't isolated.

## The processor module

A standalone wasm instantiated by our own `AudioWorkletProcessor`. It links with `-sSHARED_MEMORY` and no
`-pthread`, so libc++abi's mutex is stubbed in module.cpp.

- `configure(sr, params*)` from the worklet's init message, not from `process` (it allocates). A rate change is
  a new context, so a new configure.
- `process(frames)` per quantum: the same event loop as the native wrappers. Sized for a maximum render quantum.
- `Render_mode::Realtime` always (an `OfflineAudioContext` bounce is a later, cheap extra).
- **Latency.** Nothing to negotiate: a proposal is accepted at once and lands as `Reset::Latency` next block.
- **Timeline.** The page sets tempo, playing and a position anchored to a context frame; the module derives each
  quantum's position from its own frame counter.

The plug-in's `<P>_lib` compiles unchanged: a static archive only contributes the objects a module references, so
the processor module never links `editor.o` or Skia.

## The controller module

The CLAP wrapper's logic with the processor across the wire. Page edits are host automation; editor edits go
through the action queue with undo. A page `setState` is a host load: one undo step and
`Editor::notify(Host_preset_loaded)`, landing in the processor's `on_session_load`. `Task_manager` and
`Worker_runner` run on a prewarmed pthread pool (thread start in a browser is asynchronous). `tick()` stands in for
the view loop while no editor is open.

## Platform

`Platform_view` is a `<canvas>`; input is pointer events (mouse and touch through one path), wheel, keys; the draw
loop is rAF; `Window_context` rasterises into a buffer and `putImageData`s one reused `ImageData`; dark mode from
`matchMedia`; `Platform_paths` under an IndexedDB mount; browser dialogs (file input, downloads, HTML overlays).
No system fonts: the demos draw with `tiny_ui`'s pixel font; products embed theirs.

## Measured (2026-10-20, Apple Silicon, Chrome headless, 2x pixel density)

| | Raw | Brotli |
|---|---|---|
| processor module | 115-147 KB | 39-49 KB |
| controller module (editor + Skia CPU raster) | 2.78-2.86 MB | 739-755 KB |

Main thread with an editor open at 60 fps: 8 % (Gain) to 11 % (Block, Worker), mostly Skia rasterising the whole
editor each frame. The processor module renders bit-identical to its own native build for nine demos.

## Next

By hand:
- **The real page.** A `build-web-nonisolated` bundle on a sketch_site test page, next to the Moonbase storefront
  and a YouTube embed, under `astro dev` with no headers. The check the variant exists for.
- **Every demo in Safari (macOS)**, nonisolated: `node tools/web/serve.mjs --build build-web-nonisolated --no-isolation`,
  then `editor.html?plugin=<Base>`.
- **Hidden tab**, both variants: audio keeps playing; the editor and meters catch up on return; no stuck notes.
  Confirm no browser throttles the pump's `Atomics.wait` timeout in a background tab; if one does, block on a
  futex the worklet signals each quantum.
- **iOS Safari** over HTTPS (`cloudflared tunnel --url http://localhost:8040`): crackle, and editor feel with
  background tasks on main. Mobile CPU is several times desktop's.

In code:
- **Processor-written state is untested in both variants**: no demo declares `Writers::Processor` or `Both`. A
  small one (or a test model) and a `web_split` scenario for the snapshot path.
- The nonisolated tree runs no tiny_core tests besides `tasks_nothreads_test`; a no-threads build of the threadless
  ones would be cheap.
- Windows: the wire move (AAX through `wire.hpp`) hasn't been built on MSVC; `windows-asan-hosts` before the next
  Windows release from main.

## Later

- **Graphite on WebGPU** (`skia_use_dawn`, `skia_use_webgpu`), which Chrome and current Safari ship; removes most
  of the main-thread cost. Ganesh on WebGL2 only as a fallback.
- **Size.** A product that decodes no images could build Skia without its codecs. If the controller is still too
  big for a page, the processor module with a lighter page UI against the manifest is a valid end state.
- **Scheduled page automation.** Page `setParam` lands at the next quantum; an API mapping `AudioContext` time to a
  frame offset can follow.
- **CI.** The `web` preset in the release CI (`all_plugins/plans/ci-build-system.md`), versioned artifacts per tag;
  spk, vsl, sketch_dsp get a job there so a native-only assumption fails in CI, not in a product.

## Rules this format taught

- **Seqlocks need atomic data in wasm.** Binaryen strips `atomic.fence` at -O2, and V8 compiles atomics on ARM as
  `ldar` / `stlr`, which let a plain copy drift past them. `wire::Block_store` copies its slots as relaxed atomic
  words and `wire.js` as per-word `Atomics`; with plain copies the Release build accepted torn frames.
  `publish_with`'s in-place fill stays plain, so it is for native readers only.
- **Never block main on a task that proxies to main.** `Task_manager::shutdown` waits by spinning in Emscripten,
  and a task calling back into main deadlocks the tab. Natively the same rule, but there it only stalls.
