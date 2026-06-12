# Plan: Linux feasibility & pathway (CLAP + VST3, eye toward LV2)

> Status: **feasibility analysis / pathway.** High-level — enough to judge
> viability and sequence the work, not a line-level implementation spec. Covers
> both **tinyplug** and **[tiny_deps](../../tiny_deps)** (Skia must build on
> Linux). Initial formats: **CLAP** and **VST3**; LV2 is kept in view but deferred.

## Verdict

**Feasible, medium effort, low fundamental risk — concentrated in one new
subsystem.** Every hard dependency already supports Linux: Skia is Chromium's
native platform (the build is *easier* on Linux than the Apple/MSVC targets), the
vendored VST3 SDK ships Linux `IRunLoop`/X11 support
([iplugview.h](../../tiny_deps/third_party/vst3sdk/pluginterfaces/gui/iplugview.h)),
and CLAP is cross-platform by design. The framework's core (processor, params,
meters, worker, state, events) is already OS-agnostic; only the **editor/window
layer** is macOS/Windows-bound today. The one genuinely *new* architectural piece
is the **host-driven event loop** (Linux plug-ins don't own their UI run loop) —
that's where the design attention goes.

A large shortcut falls out of the [headless-plugin](headless-plugin.md) plan:
**headless CLAP/VST3 on Linux need no platform layer, no Skia, and no event loop
at all** — so a headless-only Linux build is nearly free and is the natural first
milestone (see "Headless shortcut").

## Current platform surface (what's portable vs. not)

| Layer | State on Linux |
|---|---|
| Core ([libs/tinyplug](../libs/tinyplug)) — processor/params/meters/worker/state/events | **Portable as-is** (pure C++20, no OS calls in the hot path) |
| [platform_defs.hpp](../libs/tinyplug/include/tinyplug/platform_defs.hpp) | `#else #error "Unsupported platform."` — needs a `__linux__` branch |
| [tiny_platform](../libs/tiny_platform) — view/dialogs/paths/window | macOS + iOS + Windows backends only; **needs a Linux backend** |
| Skia (tiny_deps) | mac/ios/win build scripts; **no Linux script**, no `setup_skia` Linux branch |
| CLAP wrapper | uses `window->cocoa`/`win32` ([view.cpp](../wrappers/clap/source/view.cpp)); **needs X11 + host event-loop extensions** |
| VST3 wrapper | `kPlatformTypeNSView`/`HWND` ([view.cpp](../wrappers/vst3/source/view.cpp)); **needs X11EmbedWindowID + IRunLoop** |
| AAX / AUv2 / AUv3 | Apple/Win only — **out of scope on Linux** (gate them out) |

## The four work pillars

### 1. Skia on Linux (tiny_deps) — lowest risk

The existing [build-skia-mac.sh](../../tiny_deps/build-skia-mac.sh) is the template:
depot_tools → `git-sync-deps` → `gn gen` → `ninja`, emitting the same static-lib
set. Add **`build-skia-linux.sh`** with Linux GN args:

```
target_os = "linux"   target_cpu = "x64"
skia_use_gl = true               # GL backend (CPU raster also available)
skia_use_fontconfig = true       # native font discovery
skia_use_freetype = true
skia_enable_gpu = true
cc = "clang"  cxx = "clang++"
# bundle the rest (libpng/zlib/expat/harfbuzz) as the mac script already does
```

Then add an `elseif(UNIX AND NOT APPLE)` branch to
[setup_skia.cmake](../../tiny_deps/cmake/setup_skia.cmake) pointing at
`build/linux/lib`, and link the Linux system deps Skia needs (`GL`, `fontconfig`,
`freetype`, `X11`, `dl`, `pthread`). **Decision:** system fontconfig/freetype
(smaller, integrates with the user's fonts) vs. fully bundled (hermetic). Recommend
**system fontconfig/freetype** — standard for Linux audio software.

*Risk:* low. Main cost is build time/size → cache the prebuilt libs in CI.

### 2. tiny_platform Linux backend — the crux

- [platform_defs.hpp](../libs/tinyplug/include/tinyplug/platform_defs.hpp): add
  `#elif defined(__linux__) → TINY_PLATFORM_LINUX 1`.
- New sources mirroring the Windows set: `linux_view.cpp`, `linux_paths.cpp`,
  `linux_dialogs.cpp`, `linux_context.cpp`, implementing the existing interfaces
  ([platform_view.hpp](../libs/tiny_platform/include/tiny_platform/platform_view.hpp),
  `platform_paths.hpp`, `platform_dialogs.hpp`, `window_context.hpp`,
  `view_delegate.hpp`). Add a Linux branch to
  [tiny_platform/CMakeLists.txt](../libs/tiny_platform/CMakeLists.txt).
- **Window/surface:** create a child **X11** window, reparented into the host's
  parent window. For the Skia surface, **start with CPU raster**
  (`SkSurface` → `XShmPutImage`), exactly mirroring the current Windows path
  (`WIN_GRAPHICS_GPU` defaults off); add a GL/EGL surface later. This keeps the
  first milestone small and reuses the proven software-raster model.
- **Paths:** XDG base dirs (`$XDG_CONFIG_HOME`, `~/.local/share`) — trivial.
- **Dialogs:** Linux has no single native file dialog — see the dedicated note
  below. **Stub/defer** initially regardless of choice; file pickers are only
  needed by the editor-origin acquire in [buffer-system.md](buffer-system.md), not
  by MVP effects or processor-origin content.

#### Dialogs: osdialog vs. xdg-desktop-portal

Two realistic paths. The decisive axis is **where the dialog renders** —
*in-process* (links a UI toolkit into the host) is the dangerous option for a
plug-in; *out-of-process* is safe.

[**osdialog**](https://github.com/AndrewBelt/osdialog) (zlib, single-file,
GTK *or* zenity on Linux; battle-tested in VCV Rack):
- *Pros:* tiny and trivially vendorable into tiny_deps; already cross-platform; the
  **zenity** mode fork/execs a separate process, so it needs **no GTK link** and
  gets the same out-of-process safety as portal with far less code; quickest MVP.
- *Cons:* its **GTK3** mode links a full toolkit into the host process — a classic
  plug-in hazard (GTK global init, its own glib main loop, and hard crashes when
  the host already loaded a different GTK/Qt). The **zenity** mode avoids that but
  requires the `zenity` binary to be installed (common on desktops, absent on
  minimal/headless systems) and can have focus/modality quirks when embedded.

**xdg-desktop-portal** (D-Bus to a portal service):
- *Pros:* renders the dialog in the portal's own process — **no toolkit linked into
  the host**, sandbox/Flatpak-friendly, Wayland-friendly; the modern standard.
- *Cons:* more plumbing (async D-Bus call integrated with the event loop) and needs
  a portal backend running (present on modern desktops, not on bare/old ones).

**Recommendation:** wrap dialogs behind the existing
[platform_dialogs.hpp](../libs/tiny_platform/include/tiny_platform/platform_dialogs.hpp)
interface and pick the backend independently. **Vendor osdialog in *zenity-only*
mode for the MVP** (out-of-process, no GTK link, minimal code), and add a
**portal** backend as the robust default later. **Avoid osdialog's GTK-linked mode
entirely** inside the plug-in — that's the one option that risks the host.

### 3. Wrapper Linux support — CLAP first, then VST3

**CLAP (recommended first, simpler):**
- `set_parent` ([view.cpp](../wrappers/clap/source/view.cpp)): handle
  `window->x11` (an X11 `Window` XID); advertise `CLAP_WINDOW_API_X11`.
- Implement the host event-loop extensions: **`clap.posix-fd-support`** (register
  the X11 connection fd) + **`clap.timer-support`** (periodic tick that drives
  `run_frame`). This is the Linux analogue of the self-driven vsync loop.
- Validate with `clap-validator` and a Linux CLAP host (Bitwig, Qtractor, Reaper).

**VST3 (second, more involved):**
- `isPlatformTypeSupported`/`attached` ([view.cpp](../wrappers/vst3/source/view.cpp)):
  add `kPlatformTypeX11EmbedWindowID`.
- Implement **`IRunLoop`** integration: obtain it from the host's `IPlugFrame`,
  register an `IEventHandler` for the X11 fd and an `ITimerHandler` for the frame
  tick. The two-component (`kDistributable`) model is unchanged — only the view's
  loop wiring is new.
- Validate with `pluginval` and Reaper-Linux.

### 4. Build / CI / presets

- **Format gating:** the existing `debug` preset condition is "not Windows", so it
  already runs on Linux but would try to build every format. Gate so **only CLAP +
  VST3** build on Linux (early-return in `make_aax/auv2/auv3_plugin.cmake` on
  non-supported OS, the way CLAP already early-returns on iOS).
- Add a **`linux`** CMake preset (Ninja/Makefiles) to
  [CMakePresets.json](../CMakePresets.json).
- CI: an Ubuntu runner, clang + Ninja, with the Skia build cached as a prebuilt
  artifact (it dominates build time).

## The event-loop crux (central design change)

On macOS/Windows the view drives its own redraw (vsync loop / timer the plug-in
owns). **On Linux the host owns the event loop** and the plug-in must register
into it — VST3 via `IRunLoop` (event handler on the X11 fd + timer), CLAP via
`posix-fd-support` + `timer-support`. So the Linux `Platform_view` must:

1. expose its X11 connection **fd** for the host to poll, and
2. expose a **`tick()`** the host's timer calls, which pumps pending X11 events
   and runs one `run_frame`.

This inverts ownership of the frame loop for Linux only. It's the one place the
existing view abstraction needs a new seam (a host-pumped variant alongside the
self-driven one) — worth designing deliberately rather than bolting on per wrapper.

## Headless shortcut (the cheap early win)

Per [headless-plugin.md](headless-plugin.md), a headless plug-in links **no
`tiny_platform`, no Skia, no view sources** and has **no event loop**. Therefore:

- **Headless CLAP/VST3 on Linux only needs pillars 1-is-skipped, 2-is-skipped,
  and the *non-view* parts of pillar 3 + pillar 4.** No X11, no Skia, no IRunLoop.
- This is achievable almost immediately once the core compiles on Linux (it
  already should) and the two wrappers' non-view code builds. **Recommend shipping
  a headless-only Linux build as Phase 1** — it proves the toolchain, core
  portability, and format wrappers end-to-end before any GUI work.

## Phased pathway

| Phase | Deliverable | Gates the next |
|---|---|---|
| **0. Spikes** | Build Skia on Linux to static libs; stand up a bare X11 child window painting an `SkSurface` rectangle; prove `IRunLoop`/`posix-fd` registration in a toy host | de-risks pillars 1 & 2 & the event loop |
| **1. Headless Linux** | Core + CLAP + VST3 **headless** building & loading on Linux (no Skia/platform) | proves core portability + format wrappers |
| **2. Skia + setup_skia** | `build-skia-linux.sh` + cmake branch; `tiny::skia` available on Linux | unblocks the GUI |
| **3. tiny_platform Linux** | X11 CPU-raster `Platform_view`, paths, stubbed dialogs, host-pumped frame loop | unblocks editor wrappers |
| **4. CLAP GUI** | CLAP X11 + posix-fd + timer; passes clap-validator; runs in Bitwig | first full GUI plug-in |
| **5. VST3 GUI** | VST3 X11EmbedWindowID + IRunLoop; passes pluginval; runs in Reaper | format parity |
| **6. Polish** | GL/EGL surface, portal file dialogs, Wayland investigation | quality |
| **7+. LV2** | New `wrappers/lv2` (see below) | format expansion |

## LV2 (future — what it adds on top)

LV2 is a separate format but **reuses the entire Linux foundation** (Skia, the X11
platform backend, the host-pumped event loop). Once Phases 2–5 land, LV2 is "just
another wrapper." Its distinctive pieces, at a glance:

- **Turtle (`.ttl`) manifests** describing ports/params — a generator under
  `tools/` (analogous to the AAX page-table generator), driven by the existing
  `Param_model`/`Meter_model` trees.
- **Port model:** control ports (params/meters), audio ports, and **atom ports**
  (for MIDI / large data — maps onto the [midi-support](midi-support.md) and
  [buffer-system](buffer-system.md) work) with **URID mapping**.
- **UI extension** (`lv2:ui`, `X11UI` type) embeds an X11 window — same surface
  the platform backend already provides.
- Worker via the LV2 `work` extension (maps onto the existing worker channel).

No new platform work; mostly manifest generation + a port/atom adapter. Reasonable
as a follow-on once CLAP/VST3 Linux are solid.

## Risks & open questions

- **Event-loop inversion** (above) is the main architectural novelty — design the
  host-pumped frame-loop seam once, shared by CLAP/VST3/LV2.
- **X11 only.** Wayland plug-in embedding is largely unsolved across the plug-in
  ecosystem; hosts still embed via X11/XEmbed (XWayland under Wayland sessions).
  Ship X11; treat Wayland as research.
- **Skia ↔ system libs** (fontconfig/freetype/GL versions) — pin and document; CI
  controls the matrix.
- **Dialogs** — osdialog (zenity mode) for the MVP, portal as the robust default;
  never link GTK into the host (see the Dialogs note). Deferred; fine for MVP.
- **Distro/toolchain matrix** — pick a baseline (e.g. Ubuntu 22.04 / glibc) and a
  compiler; static-link Skia to avoid runtime coupling.

## First concrete steps

1. **Spike Skia/Linux:** write `build-skia-linux.sh` from the mac script, build the
   static libs on an Ubuntu box, confirm sizes/deps.
2. **Spike core portability:** `cmake` the core + CLAP/VST3 **headless** on Linux;
   fix whatever isn't OS-agnostic (likely trivial). This is Phase 1 and the fastest
   proof of life.
3. **Spike X11+Skia+IRunLoop:** minimal child-window + CPU `SkSurface` + a toy
   timer/fd registration, to validate the event-loop seam before wiring it into
   `tiny_platform`.

These three spikes resolve all the real unknowns; everything after is
known-shaped engineering.
