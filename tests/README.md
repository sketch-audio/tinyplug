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
