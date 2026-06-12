# Plan: Bounce detection — offline (non-realtime) render detection

> Status: **exploratory.** Small, self-contained feature. Lets a plug-in author
> learn, per `process` call, whether the host is rendering **offline** (bounce /
> freeze / export) rather than in real time — so DSP can switch to a
> higher-quality / non-realtime-safe path (more oversampling, look-ahead,
> heavier analysis) during a bounce.

## Context

Every host has a notion of "render this faster-than-realtime to disk." Plug-ins
commonly expose a "HQ when bouncing" behaviour; this is the hook that enables it.
All five formats expose the signal, by four slightly different mechanisms:

| Format | Mechanism | Where | Granularity |
|---|---|---|---|
| VST3 | `ProcessData::processMode` (`kRealtime`/`kPrefetch`/`kOffline`) | per `process` call | per-block (free) |
| CLAP | `clap.render` extension — `renderSetMode(mode)` | host call, off audio thread | sticky flag |
| AUv2 | `kAudioUnitProperty_OfflineRender` (UInt32 bool, id 37) | host sets property | sticky flag |
| AUv3 | `AUAudioUnit.isRenderingOffline` (BOOL) | host sets property | sticky flag |
| AAX | `AAX_eNotificationEvent_{Entering,Exiting}OfflineMode` | host notification | sticky flag |

Confirmed in the vendored SDKs under [../tiny_deps/third_party](../../tiny_deps/third_party):
- VST3 `ivstaudioprocessor.h` — `ProcessModes { kRealtime, kPrefetch, kOffline }`;
  the doc explicitly says to read `processMode` of `ProcessData` per block.
- CLAP `clap/ext/render.h` — `CLAP_RENDER_REALTIME=0`, `CLAP_RENDER_OFFLINE=1`;
  clap-helpers `plugin.hh` exposes `implementsRender()` / `renderSetMode()` /
  `renderHasHardRealtimeRequirement()`.
- AAX `AAX_Enums.h` — `AAX_eNotificationEvent_EnteringOfflineMode = 'AXof'` /
  `ExitingOfflineMode = 'AXox'` ("offline bounce", Pro Tools 11+, data: none).
- AU — `kAudioUnitProperty_OfflineRender` / `isRenderingOffline` are system
  (CoreAudio / AudioToolbox) symbols; the vendored `AudioUnitSDK` does **not**
  handle the property for us, so AUv2 handles it explicitly.

## Client-side API

The render mode is a per-process property, not musical timing, so it lives on
`Dsp_context` (not `Musical_context`). Always present, defaulting to `realtime`,
so a host that never signals offline behaves correctly and existing plug-ins are
unaffected (no model, no opt-in, no `if constexpr` — it's one field).

In [tiny_processor.hpp](../libs/tinyplug/include/tinyplug/tiny_processor.hpp):

```cpp
enum class Render_mode { realtime, offline };

struct Dsp_context {
    Musical_context musical_context{};
    std::span<const float*> ibuffers{};
    std::span<const float*> sbuffers{};
    std::span<float*> obuffers{};
    size_t num_frames{};
    std::span<float> meters{};
    std::optional<uint32_t> propose_latency{};
    Render_mode render_mode{Render_mode::realtime};   // NEW

    [[nodiscard]] auto is_offline() const noexcept -> bool
    { return render_mode == Render_mode::offline; }
};
```

Author usage (pull, same style as `musical_context`):

```cpp
auto Plug_processor::process(Dsp_context& ctx) -> void
{
    if (ctx.is_offline())
        _oversampler.set_factor(8);     // HQ path during a bounce
    else
        _oversampler.set_factor(2);
    // ...
}
```

**Optional push hook (extension, concept-detected like worker replies / host
events).** Offline status changes rarely and only at block boundaries, so the
pull field is sufficient; but for authors who want to (re)allocate or reconfigure
on the transition rather than test every block, add an opt-in:

```cpp
// Detected by concept; called from the audio thread between process calls when the mode flips.
auto on_render_mode_changed(Render_mode) -> void;
```

The framework tracks the last delivered mode per instance and invokes the hook on
change. Recommend shipping the pull field first; add the hook only if a concrete
plug-in wants the transition edge.

## Per-format implementation sketch

A shared pattern for the four "sticky flag" formats: a `std::atomic<bool>
_offline{false}` on the wrapper, **set off the audio thread** (host call /
property / notification) and **read on the audio thread** in the context build
(`acquire`/`relaxed` is enough — it's a single bool, no ordering dependency on
other state). VST3 needs no atomic because the value rides `ProcessData`.

### VST3 — `ProcessData::processMode` (cleanest; per-block, no state)

In [audio_effect.cpp](../wrappers/vst3/source/audio_effect.cpp), inside the
per-segment process lambda where `context.musical_context` is built (~L304):

```cpp
using PM = Steinberg::Vst::ProcessModes;
context.render_mode = (data.processMode == PM::kOffline)
    ? Render_mode::offline : Render_mode::realtime;   // kRealtime + kPrefetch → realtime
```

`kPrefetch` (sampler pre-roll / variable-rate timeline playback) is **not** a
bounce — map it to `realtime`. No `setupProcessing` change needed;
`ProcessData::processMode` is authoritative per block (realtime↔prefetch can flip
on the RT thread, realtime↔offline flips via a host `setActive` cycle — both are
already reflected in `data`).

### CLAP — `clap.render` extension

In [plugin.hpp](../wrappers/clap/source/plugin.hpp) add `std::atomic<bool>
_offline{false}`. In [plugin.cpp](../wrappers/clap/source/plugin.cpp) override the
clap-helpers virtuals:

```cpp
auto Plugin::implementsRender() const noexcept -> bool { return true; }
auto Plugin::renderHasHardRealtimeRequirement() noexcept -> bool { return false; }
auto Plugin::renderSetMode(clap_plugin_render_mode mode) noexcept -> bool {
    _offline.store(mode == CLAP_RENDER_OFFLINE, std::memory_order_relaxed);
    return true;
}
```

In `Plugin::process` (~L65, beside the transport/musical build):

```cpp
context.render_mode = _offline.load(std::memory_order_relaxed)
    ? Render_mode::offline : Render_mode::realtime;
```

`implementsRender()` returning true auto-advertises `CLAP_EXT_RENDER` via the
helper's `clapPluginGetExtension`.

### AUv2 — `kAudioUnitProperty_OfflineRender` (id 37)

The vendored ausdk doesn't manage this property, so handle it in
[effect.cpp](../wrappers/auv2/source/effect.cpp). Add `std::atomic<bool>
_offline{false}` to [effect.hpp](../wrappers/auv2/source/effect.hpp).

- `GetPropertyInfo` (~L78): `case kAudioUnitProperty_OfflineRender:`
  `outDataSize = sizeof(UInt32); outWritable = true; return noErr;`
- `GetProperty` (~L116): write `*(UInt32*)outData = _offline ? 1 : 0;`
- `SetProperty` (~L179): `_offline.store(*(const UInt32*)inData != 0, relaxed);`
- `Render` (~L977): `context.render_mode = _offline.load(relaxed) ?
  Render_mode::offline : Render_mode::realtime;`

The host sets the property to 1 before an offline render and 0 after.

### AUv3 — `AUAudioUnit.isRenderingOffline`

The render block deliberately captures only the kernel (not `self`), so don't read
`self.isRenderingOffline` on the audio thread. Instead push the flag into the
kernel via an override, mirroring how `setMusicalContextBlock` is pushed.

In [DSPKernel.hpp](../wrappers/auv3/source/extension/DSPKernel.hpp): add
`std::atomic<bool> _offline{false};`, a `void setOffline(bool b){ _offline.store(b,
std::memory_order_relaxed); }`, and in `process` (~L126, beside
`resolve_musical_context`):

```cpp
context.render_mode = _offline.load(std::memory_order_relaxed)
    ? tiny::Render_mode::offline : tiny::Render_mode::realtime;
```

In [audio_unit.mm](../wrappers/auv3/source/extension/audio_unit.mm), override the
host-set property:

```objc
- (void)setRenderingOffline:(BOOL)flag {
    [super setRenderingOffline:flag];
    _kernel.setOffline(flag);
}
```

(`renderingOffline` is readwrite on `AUAudioUnit`; the host sets it before/after an
offline render.)

### AAX — `Entering/ExitingOfflineMode` notifications

The wrapper already has a `NotificationReceived` handler that processes
`SignalLatencyChanged` and `TransportStateChanged`
([parameters.cpp:199](../wrappers/aax/source/parameters.cpp)). Add two cases and a
`std::atomic<bool> _offline{false}` on the wrapper:

```cpp
case AAX_eNotificationEvent_EnteringOfflineMode:
    _offline.store(true,  std::memory_order_relaxed); break;
case AAX_eNotificationEvent_ExitingOfflineMode:
    _offline.store(false, std::memory_order_relaxed); break;
```

Read it in `RenderAudio` (~L513) where the context is built (~L634):

```cpp
context.render_mode = _offline.load(std::memory_order_relaxed)
    ? Render_mode::offline : Render_mode::realtime;
```

## Integration plan

1. **Core type** — add `Render_mode` enum + `render_mode` field + `is_offline()`
   to `Dsp_context` in
   [tiny_processor.hpp](../libs/tinyplug/include/tinyplug/tiny_processor.hpp).
   Default `realtime`. This alone compiles everywhere with no behaviour change
   (the field is just unset → realtime).
2. **Wrappers** — set `context.render_mode` in each of the five context-build
   sites listed above. VST3 reads `data`; the other four add one atomic each.
3. **Optional push hook** — if desired, detect `on_render_mode_changed` by concept
   in the shared drain path and call it from the audio thread when the mode flips
   (track last-seen mode per instance). Gate behind the concept so plug-ins that
   don't implement it pay nothing.
4. **Demo/test** — extend a demo to e.g. light a meter or change oversampling when
   `is_offline()`, to eyeball it in each host.

No new model, no `state_rules` change, nothing persisted — render mode is
ephemeral per-session state, never serialized.

## Semantics & edge cases

- **Default is realtime.** Hosts that never signal offline (or older versions)
  simply stay realtime — best-effort, fail-safe.
- **VST3 `kPrefetch` → realtime.** Prefetch is variable-rate timeline playback
  (sampler pre-roll), not a bounce. If a plug-in ever needs the 3-way distinction
  we can widen `Render_mode`, but bounce detection wants the 2-state split.
- **Thread safety.** The four flag-based formats set the flag on a host/main
  thread and read it on the audio thread → `std::atomic<bool>`, relaxed is fine
  (no dependent state). VST3 needs nothing (value is in `ProcessData`).
- **Mid-stream changes** only happen at block boundaries; the per-block read is
  always current. The flag is not reset by `reset(sr)` — it reflects host state,
  which the host re-asserts around the bounce.

## Limitations

- **AAX** offline notifications require **Pro Tools 11+**. Also, Pro Tools' fully
  offline *AudioSuite* processing is a *separate plug-in type* (`AAX_IHostProcessor`)
  that tinyplug doesn't build; the notification path covers offline **bounce** of
  the real-time (Native) plug-in, which is the feature asked for.
- Host coverage is best-effort: a host that bounces without signalling offline
  will read as realtime. This matches how JUCE's `isNonRealtime()` behaves.

## Verification

1. **Existing plug-ins unchanged** — `render_mode` defaults to realtime; no
   serialized-format or behaviour change. Compiles across all five wrappers.
2. **Per-format manual check** — a demo that toggles a visible meter (or asserts)
   on `is_offline()`: bounce/freeze in Reaper/Cubase (VST3), a CLAP host, Logic
   (AUv2), an AUv3 host, and Pro Tools (AAX). Confirm true during offline render,
   false during realtime playback, and that it flips back after the bounce.
3. **Validators** — auval / pluginval / clap-validator still pass (the AUv2
   property and CLAP render extension are standard; validators may even exercise
   offline render).

## Key files

| File | Change |
|---|---|
| [libs/tinyplug/include/tinyplug/tiny_processor.hpp](../libs/tinyplug/include/tinyplug/tiny_processor.hpp) | `Render_mode` enum, `render_mode` field, `is_offline()` on `Dsp_context` |
| [wrappers/vst3/source/audio_effect.cpp](../wrappers/vst3/source/audio_effect.cpp) | Read `data.processMode` in the process lambda |
| [wrappers/clap/source/plugin.hpp](../wrappers/clap/source/plugin.hpp) / [plugin.cpp](../wrappers/clap/source/plugin.cpp) | Override render virtuals + atomic; set in `process` |
| [wrappers/auv2/source/effect.hpp](../wrappers/auv2/source/effect.hpp) / [effect.cpp](../wrappers/auv2/source/effect.cpp) | Handle `kAudioUnitProperty_OfflineRender`; set in `Render` |
| [wrappers/auv3/source/extension/DSPKernel.hpp](../wrappers/auv3/source/extension/DSPKernel.hpp) / [audio_unit.mm](../wrappers/auv3/source/extension/audio_unit.mm) | `setOffline` + atomic; override `setRenderingOffline:` |
| [wrappers/aax/source/parameters.cpp](../wrappers/aax/source/parameters.cpp) | Handle Entering/Exiting offline notifications; set in `RenderAudio` |
