# Latency Demo

Changes latency at runtime the way every processor should, and shows the handshake in flight.

- Three modes (0, 2 and 20 ms of delay). Choosing one only proposes it; the delay moves when the
  host accepts (`Reset::Latency`), to exactly what it accepted. `configure` comes up in the
  selected mode and proposes nothing.
- The editor shows what the parameter wants, the proposal still waiting (red, with how long),
  what the processor renders with (green once it matches), and the last steps. Switch quickly
  through all three to watch a proposal supersede another.
- Click replaces the input with a click on every second of the timeline. With delay
  compensation working, a bounce puts each click exactly on the second, in every mode.
