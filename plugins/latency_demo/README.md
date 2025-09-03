# Latency Demo
A plug-in that demonstrates how to report latency, change it midstream.

The plug-in:
- Starts up with a non-zero latency (0.5 ms)
- Toggles between that and a high-latency path on click. (5.0 ms)

The UI should display:
- Green when the low-latency path is active.
- Yellow when the high-latency path is active.
- Red while waiting for the host to accept a proposed latency.