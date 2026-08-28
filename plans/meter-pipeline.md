# Meter pipeline

Status: **landed in tinyplug, uncommitted.** `all_plugins` deliberately untouched;
the migration list at the bottom is the running record of what it will need.

## The problem

Five wrappers each hand-rolled the "what do we tell the editor about a meter, and
when" loop, so the policies had drifted apart. Two structural facts caused every
symptom we saw:

1. `context.meters` was a per-block scratch buffer that every wrapper **zeroed
   after each callback**. So any meter not written on a given block read zero —
   whether because `process()` was skipped (host bypass, auto-bypass, a flush) or
   because the write sat behind an `if`.
2. A value was only transmitted when it **differed from a shadow**, over a bounded
   queue. So a zero, once sent, persisted until the value moved again; a meter whose
   value never changes was transmitted exactly once in the plug-in's lifetime; and
   an update the full queue refused was recorded as delivered anyway.

The offline-bounce gate was **not** implicated. In all five formats it wrapped the
comparison *and* the shadow advance, so nothing was recorded as sent during a bounce
and everything re-sent on the first realtime block. Bouncing alone could not strand
a meter. What a bounce does is get the editor closed and reopened.

## What landed

Two pieces, both in `libs/tinyplug/include/tinyplug/`.

**`meters::Publisher`** owns the scratch buffer and decides what to say each block.
Its `suppress` argument means "transmit nothing, but still reset" — an offline bounce
(Live corrupts its heap ingesting output-parameter meters during one) and a flush
block, which ran no audio and so has measured nothing. Publishing on a flush would
report a peak of zero, asserting silence that was never observed; with the mailbox's
counter that reads as a one-frame drop-out. VST3 always gated flushes; CLAP did not,
and now both do.

What survives the end of a block is the whole design:

| Policy | End of block | Sent when | Deduplicated |
|---|---|---|---|
| `Stream` | **persists** | value changed | yes — a constant should not spend traffic every block |
| `Peak` | reset | value is non-zero, plus once on the falling edge | **no** — see below |
| `Trig` | reset | value is non-zero | no — dedup would swallow a repeat |

**`meters::Mailbox`** is the editor-facing transport: one slot per address, combined
per policy, replacing the per-format `Lock_free_queue`. `Stream` stores (last wins,
retained). `Peak` takes a `max` and the reader *clears* it on read — "maximum since
you last looked" stays true whether "since" is one frame or ten minutes. `Trig`
increments a monotonic counter the reader diffs, so events neither coalesce nor need
a queue of their own; the magnitude rides alongside and is advisory.

Each slot is **one** atomic holding both the value and the count, following
`Change_list`. Two separate atomics leave a window however they are ordered: a reader
can exchange the value out, load a count the producer has not bumped yet, conclude
nothing arrived, and discard the peak it just removed. That is a lost transient, not
a delayed one. A threaded test fails reliably against the two-atomic version, so the
race is observable rather than theoretical. The `is_always_lock_free` and no-padding
asserts are load-bearing — compare_exchange compares object representation.

`Peak` carries two non-obvious interactions, both found by testing rather than by
reasoning:

1. **It must not be deduplicated.** The reader clears the slot when it looks, so an
   unchanged peak still has to be restated or a steady signal reads as silence after
   the first frame.
2. **The reader must distinguish "nothing arrived" from "silence".** A reader running
   faster than the transport delivers otherwise reads its own just-cleared slot as
   zero, and the meter flickers. VST3 makes this routine: the host forwards output
   parameters to the controller at whatever rate it likes, which need not be the
   frame rate — the symptom was a red bar flickering in the VST3 demo while AUv2 (a
   direct post from the audio thread at ~94 Hz) was clean. The slot carries a post
   counter; when it has not advanced, the reader holds its last delivered value.
   Silence is therefore announced once, on the falling edge, so "stopped" still
   reaches the reader.

## What that deleted

- the `Meter_queue` in all five wrappers (`25 × num_meters + 1` slots each)
- `Set_meter` and `Ui_event` — no remaining users
- `view::Meter_state` and its `updated` / `trigged` / `last_is_zero` bookkeeping;
  `_ui_meters` is now a plain `std::array<double, N>`. Those fields existed only to
  reconstruct per-frame coalescing from a stream of events, which the mailbox now
  does on the way in.
- a `std::vector` allocated **per frame** in `run_frame` to snapshot meter values
- VST3's `_last_meters` + `_dump_meters`, AAX's `_last_meters` + `dump_meters` —
  the mailbox *is* the cache
- `Ui_receiver::pop_meter` and `resync_meters`, replaced by one `read_meters`

Net −79 lines across 23 files, including the two new headers.

**No resync exists any more, and none is needed.** The mailbox retains every level,
so a window created at any point simply reads it. Verified: 5000 unread blocks, then
a first read returns the constant.

## Transport layout

```
VST3   processor ──output params (host-timed, unchanged)──▶ controller ──mailbox──▶ editor
AAX    algorithm ──returns ring (unchanged)──────────────▶ Parameters ──mailbox──▶ editor
AUv2   processor ─────────────────── mailbox ──────────────────────────────────────▶ editor
AUv3   ″
CLAP   ″
```

The format-imposed hop is untouched. **Output parameters stay the VST3 metering
transport** — the host owns aligning them with the audio, and rebuilding that over
`IMessage` would be work to end up somewhere worse. (An earlier note in these plans
recommended `IMessage`; that recommendation is withdrawn for metering. The Ableton
bounce crash concerned metering during *offline* render, which is suppressed.)

Caveat worth knowing: coalescing at the controller discards the per-block timing the
host handed us. Free for levels at 60 fps. For `Trig` you keep *how many* but lose
*when*, so sample-accurate trigger timing can never reach the UI through this path.

## Peaks while the editor is closed

The original question. Answer: **they are not dropped, by construction rather than by
mechanism.** `max` into a slot that is only cleared by a read means the peak over any
unread interval survives — verified over 2000 unread blocks with a single transient
in the middle. The publisher additionally holds maxima across blocks the *transport*
refused (VST3 with a null output queue, a full AAX ring), so a refusal costs latency
rather than the transient.

`Mailbox::discard()` exists for a reader that wants to drop the backlog on attach —
a stale peak and an accumulated trigger count. It is deliberately **not wired**,
because `run_frame` reduces the trigger count to "fired this frame", so a backlog
produces one frame of activity rather than a burst. Wire it if a product ever
consumes the count.

What none of this gives you is "did it clip while I wasn't looking" — that is a
**latching** meter, sticky until the editor acknowledges it, and it needs an
acknowledgement path back to the processor. A fourth policy, not a tweak to `Peak`;
a VU reading and a clip latch want opposite things on open.

## Verification

Two standalone tests in the scratchpad, **to be promoted into the repo test suite
before this is committed**:
- `meter_test.cpp` — publisher semantics in isolation (21 checks)
- `mailbox_test.cpp` — publisher + mailbox end to end (17 checks), including steady
  peaks, long unread intervals, trigger counting, refused sends, attach with no
  resync, and a reader running faster than the transport delivers

`examples/meter_demo` exercises one meter per policy in a host; its README lists what
each row proves and five host checks. Rows are identified by position and colour
(tinyplug's examples have no text rendering); the Trig row draws its last eight
magnitudes as a staircase, so a phantom trigger reads as a repeated step and a
swallowed one as a gap.

Builds clean in all five formats. **Not yet validated in a host** beyond a first
Ableton pass on the earlier queue-based version.

## AAX: Pro Tools suspends idle plug-ins (not a bug)

Worth recording, because it cost a wrong fix and will look like a meter bug again.

Symptom: in Pro Tools on a **blank** audio track, `meter_demo`'s generated rows ran
for ~10 s and froze; starting playback revived them briefly. It looked exactly like
a transport stall.

It is not. Instrumenting all four hops settled it in one run:

```
alg: blocks=301 pushed=608 refused=0 write_pos=14592   <- last line, ever
dd:  write_pos=17832 read_pos=17832 pending=0 forwarded=743 resyncs=0   <- for 22 s after
```

17832 / 24 B = 743 entries pushed, 743 forwarded, nothing pending, refused or
resynced. **The ring delivered everything and went idle; the producer stopped.**
Pro Tools stops calling render on a silent track — confirmed: with audio playing it
runs fine. It also stops redrawing the editor about 2 s later, which is why it reads
as "frozen meters" rather than "frozen window": the demo animates nothing else.

Rows 2 and 5 are generated from the processor's sample position, so they can only
advance while `process()` is being called. Put audio on the track.

**A speculative fix was written and reverted.** The hypothesis was that the producer
gated on a `read_pos` the remote consumer writes back, and so would stall forever if
that write-back were not observed. Plausible, and the ~10 s timing matched the ring
filling — but wrong, and the log shows `refused=0` throughout, so the producer was
never blocked. The change (free-running producer, consumer-owned read position,
overrun detection) traded a deliberately lossless ring for a lossy one, which is fine
for meters but weakens delivery for worker replies and latency proposals. Not worth
it for a failure mode that does not occur.

One thing from that detour is worth keeping if a lossy ring is ever wanted
deliberately: **resync to `write_pos`, never to `write_pos - capacity`.** Only a
position the producer stopped at is guaranteed to be an entry boundary; landing
mid-entry desynchronises the framing permanently, trading one stall for another. A
scratch test caught that; reasoning about it did not.

Method note: the cheap discriminator ("does it flow with audio?") would have closed
this before any code was written. Ask for it first next time.

## Migration list for `all_plugins`

Nothing here is required to compile — products just write meters as before.

1. **Stream meters now hold instead of collapsing to zero. This is a behaviour
   change, and for some meters a regression.** `scissor_hands` and `galaxy_brain`
   write `lfo_val`, `env_val`, `seq_val`, `hpf_mod`, `lpf_mod` (all `Stream`) *inside*
   the principal effect's `process()`, which the adapter skips under auto-bypass or
   when the effect is disabled. They used to drop to zero — the comment in
   `scissor_hands.hpp` (`// These are taken care of by the bypass`) shows that was
   relied on. They will now **freeze at their last value**. Fix by writing `0`
   explicitly when not modulating. Same for `hpf_mod`/`lpf_mod`, written only under
   `if (_hpf.is_processing())`.
2. **`seed_actual` is fixed for free** — an identity rather than a signal, so holding
   is correct. No action.
3. **Consider moving identity meters to `set_context`**, which runs unconditionally
   every sample before the skip decision and already receives `context.meters`.
4. **Widen clamped ranges.** VST3 normalises through `[0,1]` and clamps, so
   `sample_rate` at `Range{0, 192000}` reports 192000 in a 352.8/384 kHz session and
   `latency_samps` at `Range{0, 48000}` clamps above one second. Every product
   declares both. Append-only, no id churn.
5. **`Trig` is available and correct.** Nothing uses it yet.
6. Re-check the `quality_actual != quality_param` "not settled" test in the editors —
   it reads a `Stream` meter that used to collapse on a skipped block.

## Open

- Promote both tests into the repo test suite.
- Run `meter_demo` in hosts; five checks in its README.
- `Latch` policy, if clip indication is wanted.
- `run_frame` indexes `_ui_meters[addr]` unchecked — safe today, addresses come from
  a bounded loop.
- **`Peak`'s value and count want to be one atomic, not two.** `post` stores the value
  and then bumps the count as separate writes, so a reader landing between them takes
  the new peak while still seeing the old count, and `read` discards what it took. The
  cost is one frame reading slightly low before the next post restates it — cosmetic,
  which is why it is recorded rather than patched. But it is half an update being
  observed, and widening the `if` only papers over that. The shape that actually fixes
  it is a single atomic struct holding both fields, updated by one CAS, so there is no
  gap to land in. Do it when this code is next opened.
