# Timeline resolver: every wrapper supplies a full `Musical_context`

_2026-10-19. Landed uncommitted on `release/0.4` (for v0.4.5). `main` still to port; this file
is the porting checklist and the follow-on design for `main`._

Why: Doctor Vibe's vinyl noise looped at the buffer rate in SoundSource. That host said
"playing" while its position stood still (all_plugins `plans/frozen-host-timeline.md`). The
all_plugins adapters now ignore a stalled host position. tinyplug's part is to never hand
the processor a missing or garbage field.

## The contract (release/0.4)

`process::Timeline_resolver` (`tiny_processor.hpp`). Each wrapper fills a `Report` once per
host buffer, with `nullopt` for anything missing, failed or flagged invalid. It then calls
`resolve(report, num_frames, sr)`, and `at_offset(ctx, offset, sr)` for each sub-block.

- **Position:** the host's; else derived from host beats and host tempo; else free-running
  from the last position, reported as **not moving**.
- **Beats:** the host's; else derived from the position and tempo.
- **Tempo:** the host's if finite and > 0; else 120. **Time sig:** valid with denom > 0; else 4/4.
- **Cycle:** both ends finite; else `cycling = false`.
- While the transport is stopped, the host's playhead passes through unchanged. The
  all_plugins adapter owns the clock the DSP sees while stopped.

Test: `tests/timeline_resolver_test.cpp` (standalone, see `tests/README.md`).

## What changed per wrapper (port each to main)

| Wrapper | release/0.4 change | main today |
|---|---|---|
| AUv2 `effect.cpp` | Each host callback's `OSStatus` is checked and a failure is reported missing; the per-sub-block context comes from `at_offset`; `_timeline` member in `effect.hpp`. | `[[maybe_unused]] result` at ~1379, still dropped. |
| AUv3 `DSPKernel.hpp` | New `beginRender(start, frames)`, called once per render cycle from `AUProcessHelper::processWithEvents`; segments use `at_offset(bufferStartTime - start)`. Block BOOLs are checked. The kernel's own beat smoothing and its trailing `beat_pos +=` are gone (the all_plugins adapter does that). The position is `llround`ed. | `resolve_musical_context` per segment (~596), so every event-split segment replays the buffer start. **This was a shipping bug.** |
| VST3 `audio_effect.cpp` | Resolved once before `do_process`, using `processSetup.sampleRate`; `kProjectTimeMusicValid` / `kTempoValid` / `kTimeSigValid` / `kCycleValid` are honoured; a null `processContext` free-runs. | Built per sub-block from raw fields (~574); no flags checked. |
| CLAP `plugin.cpp` | `_resolve_transport` fills a `Report` from the `HAS_SECONDS_TIMELINE` / `HAS_BEATS_TIMELINE` / `HAS_TEMPO` / `HAS_TIME_SIGNATURE` flags; with no transport, steady time is used as the (stopped) position; `_free_run_pos` replaced by `_timeline`. | Flags ignored (~383). |
| AAX `alg_proc.cpp` | `read_musical_context(ctx, recording, st, num_frames)` reports failed queries as missing. Without ticks-per-beat, beats are now missing, where before they were raw ticks. `Alg_state` gains `sr` and `timeline`. | Old shape at ~303. |

Port order: the `tiny_processor.hpp` block and the test first, then one wrapper at a time.
Re-run each wrapper's validator, and `auval` for both AU formats.

## Then, on main only: optional fields

The goal is that the host timeline is honest about what it doesn't know.

- `Musical_context` fields become `std::optional` (position, beats, tempo, time sig, cycle).
  `Timeline_resolver::Report` is already that shape, so wrappers stop resolving and pass the
  report through.
- The defaults and derivations above move out of tinyplug into the consumer. In all_plugins,
  that is the adapters' `Host_timeline` plus their free-running clock. Each adapter decides
  what "missing" means for its DSP.
- This is a processor API change: every adapter and `Principal_effect::set_context` caller
  moves. Do it alongside the all_plugins adapter unification (the adapters still copy-paste
  their timeline block).
- A future web wrapper (`plans/web-format.md`) has no host timeline at all. It should be
  written against the optional API from the start.
