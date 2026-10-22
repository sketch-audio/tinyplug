# Contract checks

What the framework requires of a plug-in, checked on its processor and models alone, with no host. One
executable per plug-in, `<Base>_contract_checks`, registered as the CTest `contract.<Base>`:

```cmake
make_contract_checks(MyPlug_lib)
```

It links only the plug-in's library, prints a line per check and exits non-zero if any fails.
`--seed N` changes the values and histories it tries. The template calls it, and tinyplug's test tree
runs it for every demo.

The fake hosts (`tests/hosts/`) check the wrappers; these check the plug-in. A product's own state is
invisible to a wrapper test, so this is where per-product contract bugs show up.

## Checks

- **Restart** ([restart_check.hpp](restart_check.hpp)): after any history of noise and automation,
  `Restart{b}` renders bit-identically to a processor freshly configured with `b`, and a second restart
  changes nothing. When `b` implies another latency, the first block after the restart must propose it
  instead. Run from the defaults, from every range's low end to its high end and back, and from seeded
  values.

## This is where new contract checks go

Anything the framework asks of a plug-in that can be checked without a host belongs here, as one more
function in [contract_checks.cpp](contract_checks.cpp). Planned, roughly in order of value:

- **Block-size independence.** The same input and events in blocks of 1, 64 and 4096 render
  bit-identically. Hosts change buffer size between bounces, and Logic subdivides around automation.
- **Finite output** across random and extreme parameter values, the ends of every range included.
- **`configure` deterministic and re-entrant.** Configuring a used processor renders like a fresh one, and
  configuring twice is the same as once. AAX rebuilds the processor at every reset; VST3 reconfigures a
  live one.
- **Latency.** No proposal while nothing structural changes; `Latency{n}` leaves `latency_samps() == n`;
  nothing proposed above `max_latency_samps()`.
- **Realtime safety.** No allocation in `reset`, `handle` or `process` (the fake hosts' `operator new`
  trap).
- **`reset` before `configure`** is a no-op.
- **Parameter text** round trips: display, parse, display gives the same string, `Display::Custom`
  included.
- **The state record** round trips, and a newer version loads an older record.
- **The parameter lockfile** ([plans/param-lockfile.md](../../plans/param-lockfile.md)): the model matches
  a checked-in `params.lock`, with an `--update-lock` mode for deliberate changes.

Known gaps in the restart check: the bench sends no notes, so an instrument renders silence on both sides
and passes; it compares channel 0's audio only, not notes out, meters or blocks; and when the latency
differs it checks the proposal and skips the second-restart comparison.
