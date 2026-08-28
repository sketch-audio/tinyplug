# Meter Demo

One meter per policy, driven by a signal you can predict from outside the plug-in,
so a host can be checked against expected behaviour rather than "looks about right".

There is no text rendering in tinyplug's examples, so rows are identified by
position and colour, top to bottom:

| Row | Colour | Meter | Policy | What it does |
|-----|--------|-------|--------|--------------|
| 1 | red    | `peak_in`       | Peak   | Peak of the input over the block. |
| 2 | green  | `stream_lfo`    | Stream | A 0.5 Hz ramp, written every block. |
| 3 | blue   | `stream_const`  | Stream | The sample rate. Never changes after the first block. |
| 4 | yellow | `stream_sparse` | Stream | A ~3 s ramp that advances **only** while signal is present. |
| 5 | purple | `trig_pulse`    | Trig   | One event per second; a lamp plus eight history steps. |

## What each row proves

**Row 3 (blue) — resync on window open.** A meter is only transmitted when it
*changes*, so a constant is sent once in the plug-in's lifetime. Play audio, close
the plug-in window, reopen it. The bar must come straight back. If it reads zero,
the editor-attach resync is not reaching the processor in this format.

**Row 4 (yellow) — a level holds.** Play audio and it sweeps; stop, and it must
**freeze exactly where it was**, because nothing wrote it and a level is the last
value *written*, not the last value seen. It is a ramp rather than a function of
amplitude on purpose: a meter that tracked the signal would decay towards zero
along with it, and "held" would look the same as "collapsed". If this bar returns
to zero when the audio stops, the per-block scratch is being blanket-cleared again.

**Row 1 (red) — a peak decays and survives a stall.** It should track the input
and fall to zero on silence. It should also reach zero when the host bypasses the
plug-in, since no audio is being processed.

**Row 5 (purple) — trigger semantics.** The processor emits magnitudes 1,2,3…8
and repeats, one per second. The eight history steps are drawn oldest-left, with
height proportional to magnitude, so correct behaviour is a **clean repeating
staircase**. The two failure modes are visible at a glance:

- a **repeated step** means a phantom trigger — the trailing zero from the
  per-block reset is being transmitted as a second event;
- a **gap in the staircase** means a swallowed trigger — change-dedup discarded a
  repeat, or the transport dropped it.

Opening and closing the window must not add a step: an event has no current value
to restate, so a resync must skip it.

## Pro Tools: put audio on the track

Pro Tools suspends plug-ins on a silent track — it stops calling `process()` after a
few seconds, and stops redrawing the editor shortly after. Rows 2 and 5 are generated
from the processor's sample position, so they will freeze, and because this demo
animates nothing else a suspended editor is indistinguishable from a stalled meter
feed. That is host behaviour, not a bug. Run it with audio playing.

## Suggested host checks

1. Open, play audio, confirm rows 1–4 move sensibly and row 5 steps once a second.
2. Stop audio. Row 1 → 0, row 4 holds, row 3 holds.
3. Host-bypass the plug-in. Row 1 → 0; rows 3 and 4 hold.
4. Close and reopen the window. Rows 3 and 4 must come back **immediately** — the
   mailbox retains levels, so there is no resync to wait for. Row 5 may gain a
   single step: triggers that fired while the window was closed collapse into one
   on the first read. (`Mailbox::discard()` would suppress that; it is deliberately
   not wired, since `run_frame` reduces the count to "fired this frame" anyway.)
5. Offline-bounce the track, then return to realtime. Nothing should latch at zero.
