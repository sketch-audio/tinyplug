# tests

One executable per file, each registered with CTest. Three presets, which build only the tests
(`TINY_BUILD_PLUGINS=OFF`):

```sh
cmake --preset tests && cmake --build --preset tests && ctest --preset tests
cmake --preset tsan  && cmake --build --preset tsan  && ctest --preset tsan   # ThreadSanitizer
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan   # AddressSanitizer + UBSan
```

`TINY_SANITIZE` applies to the whole configure, dependencies included, and builds one
architecture: a partly instrumented link is where TSan's false reports come from.

On Windows the presets are `windows-tests`, `windows-asan` and `windows-asan-hosts` (Visual Studio,
Debug). MSVC has ASan only, no TSan or UBSan. Prebuilt Skia and AAX aren't instrumented, so STL
container annotations are off for the whole build, and the ASan runtime DLL is copied next to each
test executable.

```sh
cmake --preset windows-asan && cmake --build --preset windows-asan && ctest --preset windows-asan
```

New tests use [audio_bench](https://github.com/sketch-audio/audio_bench) (`Tests::add`,
`expect_true`, `expect_close`), fetched at a pinned commit in `tests/CMakeLists.txt`; `main` returns
non-zero when `run_all()` reports failures. The older files keep their own `expect` helper. Add a
test with one `tiny_add_test(<name>)` line. Test executables target macOS 13.3, which
audio_bench's `std::format` needs.

`tests/support/` holds shared fixtures: stand-ins for the generated `<tiny_models.hpp>`
(`generated/` with no models, `work_models/` with a work model), a queued `Pipe`, a toy undo log
and two example documents.

## Fake hosts

`tests/hosts/` loads the real plug-in bundles each demo builds and drives them through each format's
host API from the threads a real host uses, so the sanitizers can see the wrappers. Built when the
plug-ins are, by the `tsan-hosts` and `asan-hosts` presets (label `host`):

```sh
cmake --preset tsan-hosts && cmake --build --preset tsan-hosts && ctest --preset tsan-hosts
```

| Host | Loads | Notes |
|---|---|---|
| `clap_host` | `.clap` via `dlopen` | Answers `thread_check` honestly; any misbehaving or error log fails. |
| `vst3_host` | `.vst3` via `dlopen` + `bundleEntry` | Processor and controller kept apart; messages pass a proxy that, like the SDK's, delivers only on the UI thread, and an off-thread send fails. |
| `auv2_host` | `.component` via `AudioComponentRegister` | In-process only; nothing is installed. |
| `auv3_host` | a test bundle of the extension's sources (`<Demo>.auv3test`) | Units are created through the view controller, as the system does. |
| `vst3_validator` | `.vst3` | Steinberg's validator, built from source with the same sanitizer. |
| `aax_host_<Demo>` | nothing: the wrapper's sources are compiled in, one executable per demo | Fakes the controller, automation delegate, view container and private data; drives the real data model, Direct Data module, GUI and algorithm. |

Every host runs the same scenarios: scan-style create and destroy; activate/process cycles across rates,
block sizes and offline; state save/load/save identity, including streams that move a byte at a time;
truncated and corrupted state; parameter text round trips; a chaos run (audio thread processing with
automation, notes and transport while the main thread loads state, queries, flips bypass and
reconfigures); and teardown straight after activity. The AU and AAX hosts also save and load state
from another thread during chaos, and check that a load from off main reads back at once and is
then applied on main. Each audio block runs with an allocation trap
armed, and the first few allocations print their stacks. Options: `--seed N`, `--seconds S`.

The trap replaces the host's `operator new`, which is the whole process's on macOS. On Windows each
plug-in DLL links its own static CRT, so there it's an ASan malloc hook instead: every module's heap
goes through the one ASan runtime. A Windows build without ASan traps only the host's allocations.
The hook sees the OS's own heap use too, which `operator new` never does, so an allocation whose
first caller outside the ASan runtime is ntdll isn't counted: MSVC's debug STL takes a global lock
in every `vector::clear`, and a contended critical section allocates inside ntdll.

On Windows: `clap_host`, `vst3_host`, `vst3_validator` and `aax_host_<Demo>` (bundle binary via
`LoadLibrary`, VST3's `InitDll`/`ExitDll`, editors in a Win32 window, `hosts/gui_win.cpp`; AAX links
the SDK's prebuilt, uninstrumented library). `vst3_host`'s strict proxy is what proves `Relay`'s
Windows delivery is really the UI thread. Modal dialogs are cancelled as they open (a CBT hook),
since a synthetic click that opens one would otherwise park the host in its loop. Hosts raise the
timer resolution to 1 ms, as DAWs do: at the default 15.6 ms tick every short sleep in a host's
pacing rounds up, and the AAX chaos run rendered an eighth of the blocks it does at 1 ms. The
`windows-asan` test presets pass `asan.supp` (quoted, since the drive letter's colon is also
`ASAN_OPTIONS`' separator); `.gitattributes` keeps suppression files LF, which the parser needs.

Editors too, on macOS: each host opens the editor through its format's API (CLAP `gui`, VST3
`createView`/`attached`, AUv2 `kAudioUnitProperty_CocoaUI`, AUv3 the view controller's `view`) in a real
window kept at zero alpha, and feeds it synthetic clicks, drags, double and right clicks, moves and
scrolls (`hosts/gui.mm`, delivered straight to the view under the point). Scenarios: open, use and close,
idle and while processing, with state loads reaching the open editor; the editor opening and closing
during chaos; and teardown with the editor open. AUv2 and AUv3 also dispose of the unit while its view
is still in the window, which hosts do.

AAX has no host to load a bundle into (Avid's validator runs tests in a child process that loses the
preloaded sanitizer runtime), so `aax_host` stands in for Pro Tools' threads: main is host and GUI
(chunks, parameter echoes, `GenerateCoefficients`, `TimerWakeup`, notifications, resets), the algorithm
renders on an audio thread, and the Direct Data timer runs on its own. Private-data copies are acquire
loads and release stores, except the seqlocked slots (blocks, the state outbox), whose torn reads are
the protocol and are hidden from TSan. Same scenarios as above, plus `CompareActiveChunk` of a chunk
just saved, a load from off main, and chaos with three more threads saving, comparing and loading
chunks concurrently, which the SDK permits unless `AAX_eProperty_RequiresChunkCallsOnMainThread` is set.

Under a sanitizer the AU and AAX SDK libraries are compiled from source (root `CMakeLists.txt`) so
they are instrumented too. Suppressions live in `tests/sanitizers/tsan.supp`, each with its reason. Not
covered: Windows. Skia itself is prebuilt, so TSan sees its calls but not inside them.

## lock_free_queue_test.cpp

All four `Lock_free_queue` modes: exact capacity, FIFO across many trips round the storage, and
threaded delivery (nothing lost, nothing twice, in order per producer). The thread registry
refuses the thread past its limit instead of writing past its end, and `Overwrite_queue` keeps the
newest items.

## tasks_test.cpp

`Notification_queue`, `Serial_queue`, `Task_launcher`, `Task_manager` (`is_main_thread` read from
any thread while main binds, `on_main` ordering) and, on Apple, `Relay`: idle never fires, posts
coalesce onto main, nothing fires after destruction. The test pumps the main run loop for it.

## worker_runner_test.cpp

`Worker_runner`: both inbound channels in order on the worker thread, the post-cycle and update
hooks, idempotent start/stop, and 100 start/stop cycles with traffic in flight losing nothing.

## aax_transport_test.cpp

AAX's `Byte_ring` and `Block_store`, standard library only: framing, wraparound, refusal when
full, a remote reader by byte offset, a threaded ring, and the block store's seqlock stepped by
hand. The torn-frame stress runs outside TSan only, since a seqlock copy races by design.

## value_helper_test.cpp

`Value_helper` and `Host_formatter` as properties over a spread of parameter shapes: exact
endpoints, round trips between every pair of spaces, monotonic knob curves, step grids,
`knob_next`/`knob_prev`, and displayed text that re-parses to the same text.

## tiny_meters_test.cpp

`meters::Publisher` and both `meters::Mailbox` transports: stream, peak max and
silence, trig, the host transport's dropped-restatement recovery, publisher to
mailbox end to end, and one two-thread check that a peak is never lost to a concurrent
read.

## notes_test.cpp

The MIDI 1.0 codec (every message the framework reads, velocity and bend round trips, the
closed controller set, all-notes-off), note identity (two notes on one key, oldest first,
host ids, sources kept apart, editor ids, full table, release by channel), the outbox
(slice frames to block frames, ordering, capacity) and MPE (the default zone, member
expressions, inherited values, bend range, zone configuration and overlap, NRPN deselection).

## change_set_test.cpp

`Change_set`, both producer modes: coalescing, sparse iteration, empty batches, buffers
swapping roles, no allocation (with the `unordered_map` it replaced as a reference), batch
atomicity under a concurrent producer, four producers ending on the last value written to
each address, and a consumer that never waits.

## data_port_test.cpp

`Data_port` in both directions, plus `read_fresh`: coalescing, no torn reads against a
concurrent writer, and scalar handoff probes that only mean something under
`-fsanitize=thread`.

## tiny_blocks_test.cpp

`blocks::Publisher` through `blocks::Mailbox` to `blocks::Frames`: typed round trip,
`fresh` vs `latest`, an abandoned write, coalescing with the reader 1, 5, 20 and 64
publishes behind, suspend and refusal, the empty model, and a concurrent stream plus
scalar handoff probe.

## state_*_test.cpp

The state document, ported from the `tiny_state_idea` prototype:

- `state_store_test`: byte merge semantics, `state::Store` staging and triple buffer, a threaded soak, cost.
- `state_sequencer_test`: a 26 KB sequencer end to end over coupled and queued links, recording while closed, undo/redo, wire traffic.
- `state_stress_test`: ordering. Back-to-back edits, out-of-order snapshots, ABA, undo interleaving, commit gating, a 20k-op fuzz over 16 seeds, a stalled host.
- `state_retry_test`: the `Retry` policy: stale read-modify-writes, per-edit policy, coalescing, refusal is not loss, the refusal barrier (a patch diffed before the editor heard of a refusal, and a refused coalesced pair), and a fuzz over 8 seeds.
- `state_writers_test`: what each `Writers` mode removes from the API, checked at compile time.
- `state_record_test`: the persistence record: author `save`/`load` round trip, older and future author versions, truncation and foreign headers, a refused or half-done load leaving the target untouched, the raw fallback and its upgrade to functions, the stream reader, and base64.
- `state_adapter_test`: the record in preset JSON, hostile `"state"` values, reserved editor keys.
- `state_link_test`: tinyplug's `Editor_link` with the real `Undo_history`: in-process undo/redo, remote `Both` with undo around processor writes, an AAX-style reseed after reset, host loads as one undo step, AAX's resend, an Overwrite over a staged load, a stale snapshot across a load, `load_record`, and the byte budget.

The fuzzes draw with `rng() % n`, not a `<random>` distribution, whose output is implementation-defined:
with one seed and a distribution, macOS and Windows replayed different histories, and a lost-edit bug
went unseen for as long as the one libc++ history happened to pass.

