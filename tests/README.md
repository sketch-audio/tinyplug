# tests

Standalone test programs, not wired into CMake. Each is one translation unit that
builds and runs with one command, prints one line per check, and exits non-zero on
failure:

```sh
clang++ -std=c++20 -Wall -Wextra -Wconversion -Wshadow -pthread \
    -I libs/tiny_core/include -I tests tests/tiny_meters_test.cpp -o /tmp/t && /tmp/t
```

`state_link_test.cpp` includes tinyplug's interface headers, so it also needs
`-I libs/tinyplug/include -I tests/support/generated` (a stand-in for the generated
`<tiny_models.hpp>`) and nlohmann's include directory, for example
`-I build-debug/_deps/nlohmann_json-src/include`. `tests/support/` holds shared test
fixtures: a queued `Pipe`, a toy undo log and two example documents.

## tiny_meters_test.cpp

`meters::Publisher` and both `meters::Mailbox` transports: stream, peak max and
silence, trig, the host transport's dropped-restatement recovery, publisher to
mailbox end to end, and one two-thread check that a peak is never lost to a concurrent
read. Run that one under `-fsanitize=thread` too.

## change_set_test.cpp

`Change_set`, both producer modes: coalescing, sparse iteration, empty batches, buffers
swapping roles, no allocation (with the `unordered_map` it replaced as a reference), batch
atomicity under a concurrent producer, four producers ending on the last value written to
each address, and a consumer that never waits. Run it under `-fsanitize=thread` too.

## data_port_test.cpp

`Data_port` in both directions, plus `read_fresh`: coalescing, no torn reads against a
concurrent writer, and scalar handoff probes that only mean something under
`-fsanitize=thread`.

## tiny_blocks_test.cpp

`blocks::Publisher` through `blocks::Mailbox` to `blocks::Frames`: typed round trip,
`fresh` vs `latest`, an abandoned write, coalescing with the reader 1, 5, 20 and 64
publishes behind, suspend and refusal, the empty model, and a concurrent stream plus
scalar handoff probe. Run it under `-fsanitize=thread` too.

## state_*_test.cpp

The state document, ported from the `tiny_state_idea` prototype:

- `state_store_test`: byte merge semantics, `state::Store` staging and triple buffer, a threaded soak, cost.
- `state_sequencer_test`: a 26 KB sequencer end to end over coupled and queued links, recording while closed, undo/redo, wire traffic.
- `state_stress_test`: ordering. Back-to-back edits, out-of-order snapshots, ABA, undo interleaving, commit gating, a 20k-op fuzz, a stalled host.
- `state_retry_test`: the `Retry` policy: stale read-modify-writes, per-edit policy, coalescing, refusal is not loss.
- `state_writers_test`: what each `Writers` mode removes from the API, checked at compile time.
- `state_record_test`: the persistence record: author `save`/`load` round trip, older and future author versions, truncation and foreign headers, a refused or half-done load leaving the target untouched, the raw fallback and its upgrade to functions, the stream reader, and base64.
- `state_adapter_test`: the record in preset JSON, hostile `"state"` values, reserved editor keys. Links two core sources: add `libs/tiny_core/source/state_adapter.cpp libs/tiny_core/source/value_helper.cpp` and nlohmann's include directory.
- `state_link_test`: tinyplug's `Editor_link` with the real `Undo_history`: in-process undo/redo, remote `Both` with undo around processor writes, an AAX-style reseed after reset, host loads as one undo step, AAX's resend, an Overwrite over a staged load, a stale snapshot across a load, `load_record`, and the byte budget.

Run `state_store_test`, `state_stress_test` and `state_retry_test` under `-fsanitize=thread` too.
