# Latency Demo
The latency demo plug-in can be used to test that latency reporting works correctly in tinyplug.

The plug-in should:
- Start up with a non-zero latency (0.5 ms)
- Toggle between that and a high-latency path on click. (5.0 ms)

The UI should display:
- Green when the low-latency path is active.
- Yellow when the high-latency path is active.
- Red while waiting for the host to accept a proposed latency.