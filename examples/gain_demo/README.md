# Gain Demo

The smallest complete effect, and the one to copy from.

- One parameter, Gain, a linear amplitude from 0 (mute) to 4 (+12 dB). `Units::Linear_gain`
  displays it in dB everywhere, "-inf dB" at the bottom, and `Adapter::Pow{4}` gives the fader a
  dB-like feel with unity at 0.71. The kernel multiplies by the plain value directly; smoothed by
  `tiny_dsp`'s `Linear_ramp` so automation never clicks. `configure` lands on the configured value; resets snap to the target.
- Peak meters per channel before and after the gain, drawn on a dB scale with holds. Each hold
  is read out in dBFS to two decimals under its bar, to compare against the host's own peak
  meter. Click the bars or readouts to clear the holds.
