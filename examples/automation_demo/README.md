# Automation Demo

A probe for automation: what the host sends, when it lands, and what the processor makes of it.

- Four parameters, one of each kind a host automates differently: Value (continuous), Steps
  (integer), Switch (on/off), and DC Out.
- The processor records every set, ramp, `configure`, reset and render-mode change (realtime
  or offline), stamped with the frame it landed at (counted from `configure`), and follows
  host ramps sample for sample.
- The editor draws the realized Value over the last four seconds, with a tick where each event
  landed (blue set, amber ramp, red configure or reset, purple latency or render mode), and the
  log beneath. Click the log to clear it. MISSED counts entries that scrolled out before a
  frame took them.
- DC Out writes the realized Value to every output channel, so a bounce is the automation curve.
  Off, audio passes through.
- Debug builds also write each entry to the tinyplug log, category `params`:
  `log stream --predicate 'subsystem == "com.tinyplug.log" && category == "params"'`, or set
  `TINYPLUG_LOG_FILE` for a file.
