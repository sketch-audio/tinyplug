# tests

Standalone test programs, not wired into CMake. Each is one translation unit that
builds and runs with one command, prints one line per check, and exits non-zero on
failure:

```sh
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -pthread \
    -I libs/tiny_core/include tests/tiny_meters_test.cpp -o /tmp/t && /tmp/t
```

## tiny_meters_test.cpp

`meters::Publisher` and both `meters::Mailbox` transports: stream, peak max and
silence, trig, the host transport's dropped-restatement recovery, publisher to
mailbox end to end, and one two-thread check that a peak is never lost to a concurrent
read. Run that one under `-fsanitize=thread` too.

## data_port_test.cpp

`Data_port` in both directions, plus `read_fresh`: coalescing, no torn reads against a
concurrent writer, and scalar handoff probes that only mean something under
`-fsanitize=thread`.

## tiny_blocks_test.cpp

`blocks::Publisher` through `blocks::Mailbox` to `blocks::Frames`: typed round trip,
`fresh` vs `latest`, an abandoned write, coalescing with the reader 1, 5, 20 and 64
publishes behind, suspend and refusal, the empty model, and a concurrent stream plus
scalar handoff probe. Run it under `-fsanitize=thread` too.
