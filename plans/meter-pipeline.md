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
| `Peak` | reset | **every block**, zero included | **no** — see below |
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
   Which is only safe because the producer never stops restating — see "Falling
   edges" below for the version of this that shipped broken, where silence was
   announced once and the reader held a level the signal no longer had.

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

VST3 is the only leg that needs compensating for: the controller re-posts the last
delivered value per frame, because that wire is host-paced and lossy by specification.
See "Falling edges". The format-imposed hop is otherwise untouched. **Output parameters
stay the VST3 metering transport** — the host owns aligning them with the audio, and rebuilding that over
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

**Amended:** correct about the *generated* rows, and an open question for `Peak`. A
producer that stops mid-signal freezes a peak meter, which then claims signal that is
not there for the rest of the session. Whether that is reachable depends on something
this investigation did not measure: whether Pro Tools renders a stretch of silence
before it suspends. If it does, the falling edge fires during that stretch and there is
nothing to fix. If it suspends the moment a region ends, the meter freezes mid-decay.
First item under "Watch list" below — the instrumentation from this section settles it
in one run.

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

## Falling edges

A held `Peak` is the only thing in this pipeline that can be wrong *permanently*
rather than for a frame, so releasing it is worth being careful about.

`Publisher` announces silence exactly once, and `Mailbox::post` combines with `max` —
so at more than one block per read interval the announcement is absorbed by the audio
that preceded it, the publisher then goes quiet forever, and the meter holds its last
level for the rest of the silence. Reproduced at 2, 5, 20 and 64 blocks per draw; the
existing tests all ran at 1, which is the one ratio at which it cannot happen. Hosts
run at 5-20 and AAX forwards a whole 30 ms burst at a time, so this was every host.

**The fix is a deletion.** `_publish_peak` no longer special-cases zero: the guard
`if (pending == 0.f && _shadow[address] == 0.f) return;` is gone, and with it the
`_shadow` write on the Peak path. A peak is now restated every block, silent ones
included, which is what the table above already said `Peak` does — the falling-edge
special case was the one exception to a rule the design had otherwise committed to,
and the exception was the bug. `_shadow` is now Stream-only.

The reader's rule means what it says again: "nothing arrived" is a slow transport,
never a producer that stopped talking. No new state anywhere — the mailbox is
unchanged.

**Necessary everywhere, and sufficient everywhere except VST3.** Confirmed in Ableton:
VST3 and AUv2 both match the host's own peak meter digit for digit on the same audio,
and both decay to silence.

Isolated by building each half separately, which is worth recording because it pins the
mechanism rather than the symptom:

- **Reverting the publisher alone**: VST3 still works, because the controller
  compensation below manufactures the stream on its own. **AUv2 starts sticking at low
  buffer sizes** — and only at low buffer sizes, because the failure rate is a function
  of *blocks per draw*. Smaller buffers put more blocks in each UI frame, so the
  falling-edge zero is more likely to share a read interval with the audio before it and
  be absorbed by `max`. That is the model this whole section describes, observed
  directly in a host.
- **`wrappers/vst3/controller.cpp` re-posts the last delivered value each frame**, for
  `Peak` addresses only, and is what VST3 needs beyond the publisher change. The VST3
  meter wire is the one transport we do not own, and `ivstaudioprocessor.h` documents
  `outputParameterChanges` as *"optional"* with no guarantee that every point reaches
  the controller — hosts dedupe, coalesce, or sample at their own rate. `Peak` is
  defined against a value stream; the wire delivers edges. The controller turns edges
  back into a stream, which makes a delivered zero terminal.

**A third change was made and reverted**: dropping `!renders_audio` from the VST3
suppress condition, so the publisher would keep talking on flush blocks. The reasoning
was that a host switches to flush blocks exactly when the transport stops. It is
unnecessary, and the ordering is why: with the publisher restating every block, the host
has already received zeros during the silent *audio* blocks that precede the switch, so
the controller's `_last_meter` is 0 before the gate ever engages. Reverting also keeps a
genuinely idle VST3 plug-in from emitting output-parameter traffic while stopped, which
is the one place the publisher change's idle cost was not free. See the watch list for
the residual case.

The SDK's own AGain is the proof of the diagnosis: its VuMeter sends *only on change*
and its controller does nothing but `EditControllerEx1::setParamNormalized`, which
**stores**. It treats a meter as a `Stream`, so it cannot lose a falling edge and needs
nothing from the wire but edges. We treat it as a `Peak` — strictly more capable, since
we keep transients between frames and peaks accumulated while the editor is closed — and
pay for it with a requirement the wire never promised to meet. (2) is what pays that
bill, without giving up the capability.

Validator: 47 passed, 0 failed, including the parameter-accuracy passes — writing output
parameter changes at offset 0 on `numSamples == 0` blocks is legal and exercised.

**Cost.** Silence now costs what signal already cost. Mostly already paid: VST3 and
CLAP suppress the whole publish on a flush block, so an idle plug-in there still sends
nothing at all; AUv2/AUv3/CLAP pay one CAS per meter per block; AAX pays one 24-byte
ring push, and `return_ring_bytes` is already `(num_meters + 1) * 4096` — sized for
continuous per-block production at 32-sample buffers, because a steady signal was
always doing this.

**Two richer fixes were written and thrown away**, both of which work and neither of
which was worth its state. A `Slot::ended` bit in the same atomic as the count, saying
the producer's last post was zero, so the reader lands on zero on the next read: exact,
but a new bit, a new consumer-side latch, and `count_mask` on the `Trig` difference to
pay for it. And a `steady_clock` hold timeout in `read`, which additionally covers a
producer that never reaches its falling edge at all — the only mechanism that can,
since absence is not observable in band. It came out because it was the only wall-clock
behaviour in an event-driven pipeline, tuned against a number no test can pin down, and
because it *hides* what it covers: a stuck meter gets reported in a day, the same bug
behind a timeout is a one-second lag nobody files. Check 14 in the mailbox test asserts
the limitation the timeout would have removed, so re-adding it is a deliberate act.

## Watch list

Things this pass identified and deliberately did not fix, in the order they are likely
to bite. All are the same shape: `Publisher::_shadow` assumes the consumer still holds
what it last sent, `Mailbox` assumes the producer is still talking, and neither
assumption is validated — so when one breaks, the meter is wrong *permanently* rather
than for a frame.

1. **A producer that stops without reaching its falling edge.** `ended` needs one more
   block in which to say zero. Three ways it never gets one, in descending order of
   confidence:
   - a host that stops calling `process` mid-signal (Pro Tools suspends render on a
     silent track — confirmed, see above; whether it does so before or after the
     falling edge is not);
   - a stretch of flush blocks, where `!renders_audio` folds into `suppress` in
     `wrappers/vst3` and `wrappers/clap`. **Narrowed, not eliminated.** Restatement
     means the host normally receives zeros during the silent audio blocks before the
     switch to flush, so the gate is harmless. It bites only if a host switches to
     flush blocks *immediately* at the end of the last non-silent block, giving the
     publisher no silent audio block to speak in. Dropping `!renders_audio` from the
     suppress condition is the one-token fix, tried and reverted as unnecessary — take
     it only if a host demonstrates the symptom, and note it costs idle output-parameter
     traffic in VST3;
   - an AAX reset that wipes the private data block, giving a fresh `Publisher` whose
     `_shadow` is zero, against the data model's `Mailbox`, which survived.

   **Symptom to watch for: a peak meter that freezes rather than falling, and stays
   frozen until audio resumes.** If it now shows up in CLAP, the fix is the one already
   made in VST3. If it shows up in Pro Tools, it is the first bullet and the only
   remedy is a hold timeout — see "Falling edges".

   Worth running the Steinberg validator after the VST3 change: it now writes output
   parameter changes at sample offset 0 on blocks where `numSamples == 0`, which is
   legal but was not previously exercised.

2. **No resync in either direction, and `Stream` cannot self-heal from it.** A fresh
   `Mailbox` against a surviving `Publisher` reads a constant as 0 forever (VST3, if a
   host recreates the controller); a fresh `Publisher` against a surviving `Mailbox`
   leaves a stale level standing (AAX, whenever the private data block is wiped at a
   reset — which is why `construct_instance(adding_new=false)` exists). Both are
   demonstrable in a scratch rig. The fix is a periodic force-send of every meter,
   ignoring both guards, every ~500 ms of audio; it needs `publish` to take the block's
   frame count. **Decide it by logging which `alg_init` branch a Pro Tools reset takes**
   — if the block is wiped every time, this is an AAX shipping blocker rather than
   insurance. **Symptom to watch for: a `Stream` meter reading 0, or a stale constant,
   that never corrects itself** — a sample rate or latency readout will show it first.

3. `Trig` traps, none of them live because nothing uses `Trig` yet: a magnitude of
   exactly `0` is silently dropped (the value doubles as the presence flag, guarded
   twice — `_publish_event` and `post`); two triggers in one block collapse to one, so
   `Sample::triggers` counts *blocks that fired*, not events, contrary to its comment;
   and the backlog `discard()` was written to drop is still delivered in one burst
   because nothing calls it.
4. `_publish_level` has no NaN guard, unlike `post`. A NaN defeats the `==` dedupe, so
   it re-sends every block and reaches the UI.
5. Promote both tests into the repo test suite.
6. Run `meter_demo` in hosts; five checks in its README.
7. `Latch` policy, if clip indication is wanted.
8. `run_frame` indexes `_ui_meters[addr]` unchecked — safe today, addresses come from
   a bounded loop.

Landed since this list was first written: `Peak`'s value and count are now a single
atomic updated by one CAS, so there is no longer a gap for a reader to land in between
them.
