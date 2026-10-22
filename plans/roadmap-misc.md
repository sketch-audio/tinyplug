# Plan: Roadmap miscellany — Windows multitouch, macOS CPU backend, Windows GPU sync

> Status: **design.** Three independent items in `tiny_platform`. Two touch the `Window_context` graphics seam.

## The graphics seam

All rendering goes through `Window_context`
([window_context.hpp](../libs/tiny_platform/include/tiny_platform/window_context.hpp)), a pimpl with
`setup / teardown / set_drawable / begin_draw / get_canvas / end_draw / on_resized`. The CPU/GPU choice is
compile-time, one `Impl` per `#if`:

| OS | Default | Alternative |
|---|---|---|
| Windows | CPU raster, `StretchDIBits` (`WIN_GRAPHICS_GPU` 0) | D3D12 Ganesh, disabled: see item 3 |
| macOS | Metal | **none**: item 2 |
| iOS | Metal (`IOS_GRAPHICS_GPU` 1) | CPU raster composited by CoreAnimation (`layer.contents`), the escape hatch from `CAMetalDisplayLink` GPU timeouts |
| Web | CPU raster, `putImageData` | Graphite on WebGPU, later ([web-format.md](web-format.md)) |

Keep the switch compile-time. A runtime `Graphics_backend { gpu, cpu, auto }` would need both impls linked and a
unified interface; revisit only once the Windows GPU path is trustworthy.

## Item 1 — Multitouch on Windows

The event model already handles several pointers: `Event::pointer_tag` and `Event_list`'s per-tag
`pointer_origins` ([tiny_input.hpp](../libs/tiny_core/include/tiny_core/tiny_input.hpp)), fed one tag per `UITouch`
on iOS and per pointer id on the web. Only [win_view.cpp](../libs/tiny_platform/source/win_view.cpp) still uses
legacy mouse messages with the default tag 0.

- `EnableMouseInPointer(TRUE)`, so mouse, pen and touch all arrive as `WM_POINTER*`; retire the `WM_LBUTTON*` /
  `WM_RBUTTON*` / `WM_MOUSEMOVE` path rather than running both.
- `WM_POINTERDOWN / UPDATE / UP / ENTER / LEAVE / CAPTURECHANGED` → `GetPointerInfo`; `pointer_tag = pointerId`;
  `ScreenToClient` and DPI as the mouse path does now.
- Buttons from `POINTER_FLAG_FIRSTBUTTON` / `SECONDBUTTON`; touch has no right button.
- `WM_POINTERCAPTURECHANGED` ends that tag's stream with `Pointer_cancel`.
- Keep `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL` and `WM_SETCURSOR`.

Risk: a host that swallows pointer messages to our child HWND; fall back to `RegisterTouchWindow` + `WM_TOUCH`.
Smallest of the three, input only.

## Item 2 — macOS CPU backend

Parity with iOS and Windows: a software path for GPU-less VMs and CI, debugging, and pixel tests against the web
build's CPU render.

- `MAC_GRAPHICS_GPU` (default 1) in [mac_config.hpp.in](../libs/tiny_platform/cmake/mac_config.hpp.in), mirroring
  `IOS_GRAPHICS_GPU`, threaded through `configure_mac_view` ([helpers.cmake](../cmake/helpers.cmake)).
- Port iOS's raster `Impl` ([ios_context.mm](../libs/tiny_platform/source/ios_context.mm)): Skia rasterises into
  bitmaps we own and the layer's `contents` takes the result. No `drawRect:`, no Metal device, drawable pool or GPU
  watchdog. `mac_view.mm` keeps its display-link driver and sets the layer up without `CAMetalLayer`.
- Link Metal only in the GPU build.

Once macOS, Windows, iOS and web all have a raster path differing only in the final blit, a shared
`Raster_present` helper (surface allocation, then an OS blit callback) is worth extracting; do it with the Linux
backend ([linux-feasibility.md](linux-feasibility.md)).

## Item 3 — Windows GPU renderer synchronization

The D3D12 path exists but is disabled. A `Vsync_loop` thread (`DwmFlush` → `InvalidateRect`) drives `WM_PAINT`,
and the draw runs on the UI thread. Problems in [win_context.cpp](../libs/tiny_platform/source/win_context.cpp):

1. `begin_draw` and `on_resized` block on `WaitForSingleObjectEx(fFenceEvent, INFINITE)` on the UI thread: a
   stalled GPU blocks the host.
2. The fence / frame-index bookkeeping is ported from Skia's standalone `D3D12WindowContext` (the PIX `10000`
   seed, commented-out stats), which assumes it owns the loop; here the OS also sends `WM_PAINT` on resize and
   expose.
3. Two pacing sources: `Present(1, 0)` and the `DwmFlush` thread.
4. `DXGI_SWAP_EFFECT_FLIP_DISCARD` on a child HWND is fragile; the robust path for child surfaces is a
   composition swapchain (`CreateSwapChainForComposition` + a DirectComposition visual).

Fix, in order:

- (a) Non-blocking wait: if `fFence->GetCompletedValue()` is behind, skip the frame (`get_canvas` already returns
  `Canvas{nullptr}` without a surface; make `end_draw` a no-op). The next tick retries.
- (b) One pacing source: `Present(0, 0)` under the `DwmFlush` thread, or drop the thread for GPU.
- (c) Audit the fence logic against current Skia's `D3D12WindowContext`; remove the debug seed and dead code.
- (d) Skip a `WM_PAINT` that arrives during `ResizeBuffers`.
- (e) A composition swapchain, if child-HWND artifacts remain.

Flip `WIN_GRAPHICS_GPU` to 1 only after Reaper, Cubase, FL and Live on Windows, across several GPUs and a
software-adapter VM: no UI-thread stalls, no tearing, correct live resize.

## Order

Windows multitouch (isolated, small), then the macOS CPU backend (a port of iOS's), then Windows GPU sync (hardest).
