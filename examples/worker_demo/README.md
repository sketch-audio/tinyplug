# Worker Demo

All four worker channels, doing a job a worker is for: work the audio thread mustn't do.

- **Processor -> worker**: when Drive moves, a request for a waveshaper curve (`Design`), plus a
  heartbeat every 100 ms.
- **Worker -> processor**: the designed curve (`Curve`), which replaces the last one at the top
  of a block, and each heartbeat echoed, which gives the round trip in frames.
- **Editor -> worker**: a ping every 250 ms while the window is open.
- **Worker -> editor**: each pong, with how many curves the worker has designed.

The editor draws the processor, the worker and itself, with a lane each way that flashes as
messages cross and counts them, plus both round-trip times. Beneath, the curve the processor is
shaping with: amber while a newer Drive is still being designed, green once it arrives.
In VST3 the worker lives with the controller, so the processor's messages cross the component
boundary.
