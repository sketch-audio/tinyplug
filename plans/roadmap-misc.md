# Plan: Roadmap miscellany — Windows multitouch, macOS CPU backend, Windows GPU sync

> Status: **design.** Three independent roadmap items, grouped because two of them
> touch the same `Window_context` graphics seam. Each has its own section + a
> shared note on the compile-time-vs-runtime CPU/GPU switch.

## Shared context: the graphics backend seam

All rendering goes through `Window_context`
([window_context.hpp](../libs/tiny_platform/include/tiny_platform/window_context.hpp))
— a pimpl with `setup / teardown / set_drawable / begin_draw / get_canvas /
end_draw / on_resized`. The view's draw loop calls `begin_draw → get_canvas →
(editor paints) → end_draw`. Backends today:

| OS | Backend | Surface | Present |
|---|---|---|---|
| Windows | **CPU only** (GPU code exists, disabled) | `SkSurfaces::WrapPixels` over an `SkBitmap` | `StretchDIBits` to the HDC |
| Windows (GPU, off) | D3D12 / Ganesh | swapchain backbuffer surfaces | `IDXGISwapChain3::Present` + fence |
| macOS | **Metal only** | `WrapBackendRenderTarget` on a `CAMetalLayer` drawable | `presentDrawable` |
| iOS | **Metal only** | same | same |

The CPU/GPU choice is **compile-time** today: `WIN_GRAPHICS_GPU` (a configured
define in [win_config.hpp.in](../libs/tiny_platform/cmake/win_config.hpp.in),
hardcoded to `0` in
[tiny_platform/CMakeLists.txt:29](../libs/tiny_platform/CMakeLists.txt) with the
comment *"GPU renderer not ready for prime time. Need to figure out some wait-free
synchronization mechanism."*). The two backends are **separate `Impl` definitions
selected by `#if`** — different members, different framework links.

---

## Item 1 — Multitouch on Windows

**The event model is already multitouch-capable; only the Windows input source
isn't feeding it.** `Event` carries a `pointer_tag`
([tiny_view.hpp:135](../libs/tinyplug/include/tinyplug/tiny_view.hpp)) and
`Event_list` tracks per-tag `pointer_origins`; **iOS already populates a distinct
tag per `UITouch`** ([ios_view.mm](../libs/tiny_platform/source/ios_view.mm)). So
**no core change is needed** — the work is confined to Windows input.

**Current state:** [win_view.cpp](../libs/tiny_platform/source/win_view.cpp) uses
legacy **mouse messages** (`WM_LBUTTONDOWN/UP/DBLCLK`, `WM_MOUSEMOVE`,
`WM_RBUTTON*`) → emits `Pointer_*` events with the **default `pointer_tag` (0)`** —
a single implicit pointer.

**Plan: adopt the Windows Pointer Input stack (`WM_POINTER*`).**

- Call `EnableMouseInPointer(TRUE)` at startup so **mouse, pen, and touch all
  arrive as unified `WM_POINTER*` messages** (one code path, mouse included).
- Handle `WM_POINTERDOWN / WM_POINTERUPDATE / WM_POINTERUP / WM_POINTERENTER /
  WM_POINTERLEAVE / WM_POINTERCAPTURECHANGED`. For each, `GET_POINTERID_WPARAM` +
  `GetPointerInfo` → `POINTER_INFO{ pointerId, ptPixelLocation (screen),
  pointerType }`.
- **Map `pointerId → pointer_tag`** (e.g. `pointer_tag = pointerId`), so two
  fingers produce two independent `Pointer_down`/`Pointer_move`/`Pointer_up`
  streams with distinct tags — which the framework's `Event_stream` /
  gesture/value handling already disambiguates per tag.
- Convert `ptPixelLocation` screen→client (`ScreenToClient`) and apply DPI exactly
  as the mouse path does now.
- **Button mapping:** mouse/pen carry left/right via `POINTER_INFO` flags
  (`POINTER_FLAG_FIRSTBUTTON` / `SECONDBUTTON`); **touch has no right button** —
  leave right-click to mouse/pen, and let the existing cross-platform gesture layer
  handle touch long-press if/when needed.
- **Capture for drags:** `WM_POINTERCAPTURECHANGED` ends a tag's stream (emit
  `Pointer_cancel` for that tag); touch capture is implicit per pointer.
- **Keep `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL`** (wheel isn't a pointer) and the
  cursor handling (`WM_SETCURSOR`) unchanged.

**Risks:** mixing legacy + pointer messages — `EnableMouseInPointer` unifies them,
so retire the `WM_LBUTTON*` path rather than running both. Validate that the
plug-in's **child HWND** (embedded in the host window) still receives pointer
messages; if a host swallows them, fall back to `RegisterTouchWindow` + `WM_TOUCH`.

**Smallest, most isolated of the three** — touches only `win_view.cpp` input, no
graphics-backend interaction.

---

## Item 2 — Apple (macOS + iOS) CPU (software) graphics backend

**Goal:** mirror Windows' compile-time CPU/GPU split on **both Apple OSes** — a
software raster backend alongside Metal — for GPU-less VMs/CI, debugging, and
parity (and it shares the raster-present idea the Linux CPU path will reuse). iOS
and macOS use the **same** present path (Core Graphics), so they're planned
together.

**Current:** [mac_context.mm](../libs/tiny_platform/source/mac_context.mm) and
[ios_context.mm](../libs/tiny_platform/source/ios_context.mm) are Metal-only —
`CAMetalLayer` + `WrapBackendRenderTarget` over a drawable + `presentDrawable`.

**Plan: add `TINY_APPLE_GRAPHICS_GPU` (default 1)** (a single flag covering mac +
iOS) to the configured headers
([mac_config.hpp.in](../libs/tiny_platform/cmake/mac_config.hpp.in) and a new iOS
equivalent), mirroring `WIN_GRAPHICS_GPU`, and a CPU `Window_context::Impl`.

**Present via Core Graphics — `SkCGDrawBitmap` (per iPlug2's `IGraphicsSkia`).**
This is the clean, shared-Apple path: render to a raster surface, then blit the
pixels into the view's current `CGContextRef`:

```cpp
#include "include/utils/mac/SkCGUtils.h"   // SkCGDrawBitmap
// end_draw():
SkPixmap pixmap;
_surface->peekPixels(&pixmap);
SkBitmap bmp;  bmp.installPixels(pixmap);
auto* cg = static_cast<CGContextRef>(_drawable);     // the view's CGContext
CGContextSaveGState(cg);
CGContextScaleCTM(cg, 1.0 / scale, 1.0 / scale);     // undo retina backing scale
SkCGDrawBitmap(cg, bmp, 0, 0);
CGContextRestoreGState(cg);
```

- **Surface:** `SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w*scale, h*scale))`
  (same raster model as the Windows CPU branch
  [win_context.cpp](../libs/tiny_platform/source/win_context.cpp)).
- **`set_drawable`** receives the **`CGContextRef`** (exactly like the Windows CPU
  path passes the `HDC`) — no longer a no-op on Apple. The view supplies it from
  its draw callback: macOS `[[NSGraphicsContext currentContext] CGContext]` in
  `drawRect:`, iOS `UIGraphicsGetCurrentContext()` in `drawRect:`.
- **No `CAMetalLayer`, no drawable handshake, no Metal** in the CPU build.
- **Views** ([mac_view.mm](../libs/tiny_platform/source/mac_view.mm),
  [ios_view.mm](../libs/tiny_platform/source/ios_view.mm)): the CPU build uses a
  plain (non-Metal) `NSView`/`UIView` that implements `drawRect:`, hands its
  `CGContextRef` to `set_drawable`, then calls the draw → `end_draw` blit. The
  redraw driver (display link / timer) invalidates the view instead of acquiring a
  Metal drawable.
- **Fonts:** the CoreText font manager (`SkFontMgr_New_CoreText`) is backend-
  independent, so no change.

**CMake:** thread `TINY_APPLE_GRAPHICS_GPU` through `configure_mac_view`
([helpers.cmake](../cmake/helpers.cmake)) and the Apple link blocks — link
`-framework Metal` only for the GPU build; the CPU build keeps QuartzCore (CALayer)
+ CoreGraphics/CoreText (already via Cocoa/UIKit). Compile `mac_context.mm` /
`ios_context.mm` **or** new `*_context_cpu.mm` files per the flag. Confirm the
tiny_deps Skia build exports the mac CG utils (`SkCGDrawBitmap` lives in Skia's
`utils/mac` port) — likely already in `libskia.a`; verify, else enable it.

**Optional refactor:** the raster-surface model is now shared by Windows + Apple
(+ Linux next), differing only in the final blit (`StretchDIBits` / `SkCGDrawBitmap`
/ X11). A small `Raster_present` helper (alloc surface, hand pixels to an OS blit
callback) could de-duplicate — worth doing when the Linux backend lands.

---

## Item 3 — Windows GPU renderer synchronization fix

**Current:** the D3D12 path exists but is disabled. Draw is driven by a separate
`Vsync_loop` thread (`DwmFlush()` → `InvalidateRect`) that posts `WM_PAINT`; the
actual draw runs on the **UI thread** in `WM_PAINT` →
`begin_draw/get_canvas/end_draw`. `begin_draw` does
`WaitForSingleObjectEx(fFenceEvent, INFINITE)` — a **blocking** wait on the UI
thread ([win_context.cpp:246](../libs/tiny_platform/source/win_context.cpp)).

**The problems:**

1. **Blocking `INFINITE` fence wait on the UI thread** — the literal "wait-free
   synchronization" the code comment asks for. A slow/stalled GPU blocks the host
   UI thread.
2. **Loop-ownership mismatch:** the fence/frame-index bookkeeping was ported from
   Skia's standalone `D3D12WindowContext` (note the `10000` PIX-tracking init and
   commented-out stats blocks), which assumes a controlled single-owner loop. Here
   `WM_PAINT` can *also* fire from the OS (resize/expose) independently of the
   vsync thread, so frames can interleave unexpectedly.
3. **Double pacing:** `Present(1, 0)` waits for vsync *and* the external
   `DwmFlush` thread also paces — two pacing sources contending.
4. **Child-HWND + flip-model swapchain (likely the deep root cause):**
   `DXGI_SWAP_EFFECT_FLIP_DISCARD` on a **child window** HWND (our editor is a
   child of the host's window) is historically fragile; the robust path for child
   surfaces is a **composition swapchain**
   (`IDXGIFactory2::CreateSwapChainForComposition` + a DirectComposition visual)
   rather than `CreateSwapChainForHwnd`.

**Fix approach (phased):**

- **(a) Non-blocking, frame-dropping wait.** In `begin_draw`, if
  `fFence->GetCompletedValue() < expected`, **skip this frame** (return no canvas;
  `get_canvas` already returns `Canvas{nullptr}` when the surface is absent — make
  `end_draw` a no-op for a skipped frame). The next vsync tick retries. This is the
  wait-free mechanism and resolves problem 1 immediately.
- **(b) One pacing source.** Since `DwmFlush` already paces the vsync thread, use
  `Present(0, 0)` (no Present vsync wait) — or drop the vsync thread for GPU and
  rely on `Present(1, 0)`. Pick one; don't pace twice.
- **(c) Audit fence/frame-index logic** against the current Skia
  `D3D12WindowContext` reference; remove the PIX-debug `10000` seed and dead
  commented code; make the per-buffer fence values provably correct.
- **(d) Serialize resize vs draw.** `on_resized` already flushes + waits + rebuilds;
  guard with an in-resize flag so a `WM_PAINT` during `ResizeBuffers` is skipped
  (consistent with (a)).
- **(e) Evaluate the composition swapchain** for child-HWND robustness (problem 4)
  — likely the durable fix once (a)–(d) make it stable enough to test.

**Phasing:** (1) (a)+(b)+(c)+(d) → blocking gone, basically correct. (2) (e)
composition swapchain if child-HWND artifacts persist. (3) flip the
`WIN_GRAPHICS_GPU` default to `1` only after validating across Reaper / Cubase /
FL / Live on Windows (multiple GPUs + a software-adapter VM).

**Verification:** instrument for UI-thread stalls (none), no tearing, correct live
resize, and a multi-host pass before enabling by default.

---

## Compile-time vs. runtime CPU/GPU switch

**Recommendation: keep it compile-time for all of this work** (`WIN_GRAPHICS_GPU`,
new `TINY_MAC_GRAPHICS_GPU`). The `Window_context::Impl` is selected by `#if` with
different members and different framework links, so a runtime switch is a larger,
separable change — not a prerequisite for items 2 and 3.

**Runtime switch (future, sketched so today's flags stay forward-compatible):**

- Compile **both** backends into the platform lib and choose the `Window_context`
  impl at construction → requires a **unified runtime interface** with two impls
  (today they're compile-time-exclusive) and **linking both** dependency sets
  (D3D12 *and* the CPU path on Windows; Metal always present on macOS) — some
  binary bloat.
- **Client-facing setting:** a `Graphics_backend { gpu, cpu, auto }` the plug-in
  declares (a `Plug_info` property or an editor hint), default `auto` → try GPU,
  fall back to CPU on init failure. The platform factory builds the matching
  `Window_context`.
- The current compile-time flags are a **strict subset** of this — the runtime
  factory would subsume them, so nothing built now is wasted.

I'd ship the compile-time work first and treat the runtime switch as a follow-on
once the GPU path is actually trustworthy (item 3).

---

## Independence & suggested order

All three are independent. Suggested sequence by effort/risk:

1. **Windows multitouch** — isolated to `win_view.cpp` input; no graphics-backend
   interaction; the event model already supports it.
2. **Apple CPU backend (mac + iOS)** — moderate; reuses the Windows CPU raster
   model with a Core Graphics (`SkCGDrawBitmap`) blit; gives parity and seeds the
   Linux CPU path.
3. **Windows GPU sync** — hardest; may need a composition swapchain; gate the
   default flip on multi-host validation.

## Key files

| Item | Files |
|---|---|
| Multitouch (Win) | [win_view.cpp](../libs/tiny_platform/source/win_view.cpp) (input messages); event model in [tiny_view.hpp](../libs/tinyplug/include/tinyplug/tiny_view.hpp) is **unchanged** |
| Apple CPU (mac + iOS) | CPU branch / new `*_context_cpu.mm` for [mac_context.mm](../libs/tiny_platform/source/mac_context.mm) + [ios_context.mm](../libs/tiny_platform/source/ios_context.mm) (`SkCGDrawBitmap` via `include/utils/mac/SkCGUtils.h`); [mac_view.mm](../libs/tiny_platform/source/mac_view.mm) + [ios_view.mm](../libs/tiny_platform/source/ios_view.mm) (non-Metal `drawRect:` supplying the `CGContextRef`); [mac_config.hpp.in](../libs/tiny_platform/cmake/mac_config.hpp.in) + iOS config + [helpers.cmake](../cmake/helpers.cmake) + [CMakeLists.txt](../libs/tiny_platform/CMakeLists.txt) (`TINY_APPLE_GRAPHICS_GPU`, conditional Metal link) |
| Windows GPU sync | [win_context.cpp](../libs/tiny_platform/source/win_context.cpp) (`begin_draw`/`end_draw`/`on_resized`/swapchain); [win_view.cpp](../libs/tiny_platform/source/win_view.cpp) (`Vsync_loop` pacing); [CMakeLists.txt:29](../libs/tiny_platform/CMakeLists.txt) (`WIN_GRAPHICS_GPU` default) |
