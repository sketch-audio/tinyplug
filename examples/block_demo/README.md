# Block Demo

A gain whose output is analysed two ways and published as **blocks**: typed frames the
processor writes and the editor draws. It has no meters, so it also exercises the
meter-free path.

| Panel | Block | Frame | Published | Editor reads |
|-------|-------|-------|-----------|--------------|
| left  | `Spectrum` | `Spectrum_frame`, 1024 bins in dBFS | every FFT hop (2048 point, 50% overlap) | `latest`, every draw |
| right | `Scope`    | `Scope_frame`, 512 samples          | every completed sweep | `fresh` only |
| strip | —          | —                                   | —                     | the Gain parameter; drag vertically |

The number under each panel is **frames that arrived in the last second**. That's the
transport's real delivery rate in this host, and the main thing to check per format.

## What it proves

**Frames arrive, typed, in every format.** Both panels move with audio. Nothing in the
editor casts or reinterprets: `latest<Block::Spectrum>()` returns a `Spectrum_frame`.

**Latest wins.** At 48 kHz the spectrum publishes ~47/s and the scope up to ~94/s, but the
editor only sees one frame per draw. Expected arrival counts:

| Format | Spectrum | Scope | Why |
|--------|----------|-------|-----|
| CLAP, AUv2, AUv3 | ~47 | ≤ frame rate | shared mailbox, read every draw |
| VST3 | ≤ 60 | ≤ 60 | relay sends at most one frame per address per 1/60 s |
| AAX | ≤ ~33 | ≤ ~33 | Direct Data wakeup, ~30 ms and irregular |

**`fresh` vs `latest`.** Stop the transport. The spectrum holds its last frame and the
peaks fall to it; the scope holds the last sweep and its counter drops to 0. Neither goes
blank: no arrivals isn't the same as a frame of silence.

**Trigger vs free-run.** The scope trace is blue when a sweep started on a rising zero
crossing and amber when it free-ran after 50 ms without one (silence, DC).

**Retained across a closed window.** Close and reopen the editor while playing: both
panels come back immediately with no resync step.

**Offline bounce.** Nothing is published while the host renders offline, so both counters
read 0 during a bounce.

**AAX reset.** Change the session sample rate or re-instantiate the plug-in. The
algorithm's stores come back empty (`seq == 0`), and the editor keeps drawing the
last frame until the next one arrives rather than flashing to zeros.
