# tests

Standalone test programs. **Not wired into CMake** — there is no harness here yet.
Each is a single translation unit with no dependencies beyond the header it covers,
so it builds and runs with one command:

```sh
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow \
    -I libs/tinyplug/include tests/meter_publisher_test.cpp -o /tmp/t && /tmp/t
```

They print one line per check and exit non-zero on failure, so they drop into a
harness later without rewriting the assertions.

## meter_publisher_test.cpp

`meters::Publisher` in isolation — what the processor says about a meter and when.
16 checks. The ones worth keeping if the file is ever trimmed:

- a `Stream` value persists across blocks that never write it (the bug that started
  all this: a blanket per-block clear made any un-written meter collapse to zero)
- a `Peak` is held across sends the transport refused, rather than lost
- a `Trig`'s trailing zero is not delivered as a second, phantom trigger
- a repeated `Trig` is not swallowed by change-detection
- an offline block still resets peaks, so a bounce cannot hoard a spike that
  flushes as one absurd value on the first realtime block
- a peak accumulated over a long interval the transport refused still arrives

## meter_mailbox_test.cpp

`Publisher` + `Mailbox` end to end — the editor-facing transport. 18 checks, one of them concurrent. The three
that encode non-obvious behaviour, both of which were real bugs first:

- **a steady signal keeps reading.** `Peak` must not be deduplicated: the reader
  clears the slot when it looks, so an unchanged peak still has to be restated or a
  held note reads as silence after one frame.
- **a reader faster than delivery does not flicker.** "Nothing arrived" is not
  "silence". VST3 makes this routine — the host forwards output parameters to the
  controller at its own rate, which need not be the frame rate. The slot carries a
  post counter; when it has not advanced the reader holds its last value, and
  silence is announced once on the falling edge so "stopped" still gets through.

- **a posted peak is never dropped.** The reason `Slot` is a single atomic rather
  than a separate value and count: a reader could exchange the value out, load a
  count the producer had not yet bumped, conclude nothing arrived, and discard the
  peak it had just removed. This is the one test that needs a second thread, and it
  fails reliably against the two-atomic version — the race is observable, not
  theoretical.

Also covered: a constant reaching a newly attached editor after 5000 unread blocks
with no resync mechanism at all, and a peak surviving 2000 unread blocks.
