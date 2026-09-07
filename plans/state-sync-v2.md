# Plan: State & synchronization v2

Three interlocking pieces of work, planned together because they share one
primitive and one store:

1. **Transactional publish** — no tearing across a frame or block boundary on any
   framework-owned channel.
2. **`state::Model`** — a declared, synchronized, undoable, persisted model for
   non-parameter state.
3. **State v2** — one canonical value store and one model↔document layer behind the
   five per-format chunk paths.

They can ship independently, but each is cheaper if the others land, and 1 and 3
share the same two foundations. Sequencing is in [Staging](#staging).

## Context

Everything the framework synchronizes for the author today is a scalar parameter.
That property — *declare the model, the framework keeps it in sync everywhere* — is
the whole premise of tinyplug, and it currently stops at `params::Model` and
`meters::Model`.

Three separate investigations converged on the same conclusion:

- A sequencer wants thousands of values that are not automatable and never should
  be. Params can express them (fixed max cardinality + per-slot `Int`), but every
  format's contract is built around *enumerating* parameters — naming, describing,
  listing, persisting them one at a time — and that cost is per-param regardless of
  `Policy`. See [param scaling](#appendix-a-parameter-scaling-measurements).
- Buffer sources, mod matrices, chain orders and drawn curves all want the same
  four services (sync, undo, persistence, transactional delivery) and none of them
  fit a `double`.
- The five state paths have drifted into **three different value spaces** and two
  incompatible identity models, and the migration logic is hand-rolled five times.

## Shared foundations

Both 1 and 3 rest on these. Build them first; they are small and independently
useful.

### F1. `Transaction<T, capacity>` — flat triple buffer

Generalizes [change_list.hpp](../libs/tinyplug/include/tinyplug/change_list.hpp),
which is already a triple-buffered batch publisher with all-or-nothing `consume`.
Three changes:

| Today | v2 |
|---|---|
| `std::unordered_map<uint32_t, double>` payload | `std::array<T, capacity>` + count |
| `std::mutex` on the push side | none (single producer per instance) |
| coalesces by address on push | coalesces by address on push, in a flat array |

`consume()` keeps its current behavior of **returning on CAS failure rather than
spinning** — the audio thread never blocks, and contention costs one block of
delay. That is the contract: *delivery may be delayed, never torn.*

```cpp
template<typename T, size_t capacity>
class Transaction {
public:
    auto begin() -> Builder;                 // Producer-side handle; one open at a time.
    auto publish(Builder&&) -> bool;         // Wait-free: fill private slot, swap index.
    template<typename F> auto consume(F&&) -> bool; // All-or-nothing; false if busy.
};
```

Removing the map also removes the allocation risk (`reserve(2 * capacity)` and
hope) and the pointer-chasing `consume` — see [A.4](#a4-audio-thread-costs).

Migrate the two existing users behind it: `Vst3_controller::_state_queue`
(`Set_param`, knob, to editor) and `Auv2_effect::_changes`
(`process::Event::Set`, plain, to audio thread). Keep the two-sided templating —
naming the event type is what keeps each instance honest about its space.

### F2. `Value_store` — the framework owns canonical parameter values

Today each wrapper borrows its format's store: AUBase `Globals()`, AAX
`AAX_IParameterManager`, VST3 `_host_values`, CLAP `_hostvalues`. **That borrowing
is why there are five state paths** — each one has to marshal a different
representation, in a different space, at a different width.

```cpp
class Value_store {                     // One per plug-in instance.
    std::array<std::atomic<double>, num_params> _plain{};
public:
    auto get(uint32_t addr) const -> double;      // Plain space, always.
    auto set(uint32_t addr, double plain) -> void;
    auto snapshot(std::span<double> out) const -> void;
};
```

Canonical space is **plain**, canonical width is **double**. Each wrapper keeps its
host mirror in sync from this, rather than the other way round. Rationale in
[Part 3](#part-3-state-v2).

## Part 1: Transactional publish

**Goal.** Where the framework owns the transport, a set of values published together
is observed together. Where it does not, say so explicitly rather than implying a
guarantee that the host can break.

### 1.1 What is and is not achievable

| Channel | Owner | Guarantee |
|---|---|---|
| Meters → editor (AAX/AUv2/AUv3/CLAP) | framework | **Atomic per block** |
| Meters → editor (VST3) | host | Best-effort; see [1.4](#14-vst3-meters) |
| State-model edits → processor | framework | **Atomic per gesture** |
| State/preset load → processor (VST3/CLAP/AUv2/AUv3) | framework | **Atomic per load** |
| State/preset load → processor (AAX) | host | Atomic after [1.5](#15-aax-segment-transactions) |
| Editor param edits → processor | **host** | **None. Cannot be provided.** |

The last row is a hard limit, not a gap. Editor param changes travel through the
host's gesture/edit API (`performEdit`, `AUParameterSet`, `clap_host_params`, AAX
`SetParameterNormalizedValue`); the host decides when they reach `process` and no
format offers "apply these N together." Most hosts deliver a UI frame's edits in one
block. Nothing in any spec says they must.

This must be **documented as a contract**, not left implicit. Authors who need
frame-atomic delivery of many values should put them in a `state::Model` slot,
which is the framework-owned path.

### 1.2 Meters: snapshot publish

Two structural sources of tearing today:

- `meters::Publisher::publish` loops calling `send(address, value)` per address.
- `meters::Mailbox::read` loops over `num_meters` independent
  `std::atomic<Slot>`s, so the editor can observe meter 3 from block *K* and meter
  7 from block *K+1*.

Replace the per-address mailbox with a whole-set snapshot through `Transaction`:

```
[audio thread]  Publisher fills a private std::array<Sample, num_meters>
                one atomic exchange publishes the whole set
[ui thread]     one atomic exchange claims the newest set; reads it uncontended
```

Writer wait-free, reader wait-free, neither spins.

**This is cheaper than what exists.** `Mailbox::post` currently runs a CAS loop per
meter per block; after, it is `num_meters` plain writes to private memory plus one
exchange. Above ~2 meters it is strictly less atomic traffic, and every CAS loop in
`Mailbox` disappears — with a single producer writing to a private buffer there is
nothing to contend with. Storage goes to `3 × num_meters × sizeof(Sample)`.

**Two semantics need rework, not porting:**

- **`Peak`'s take-and-clear.** Today the *reader* clears the slot, which a snapshot
  writer cannot observe. Replace with a monotonic `_consumed` counter the reader
  publishes and the writer reads once per block (relaxed); when it advances, the
  writer resets its peak accumulators. Preserves "maximum since you last looked"
  exactly, and stays wait-free.
- **`send(...) -> bool` and the retry path.** A snapshot transport has fixed
  capacity and cannot partially fail, so `Publisher::_pending` and the
  refused-value retry logic collapse. Careful here: the falling-edge-to-zero rule
  in `_publish_peak` depends on knowing what was actually delivered, and
  `_shadow` doubles as "was the last thing we sent a zero." That logic must be
  re-derived against the new transport, not deleted.

`suppress` semantics are unchanged and still run the resets.

### 1.3 Cross-model atomicity

Meters, state slots and blocks each get their own `Transaction`. Publishing them
*together* (one editor frame sees meters and state from the same block) is **out of
scope for v1 of this work** — it would require one composite transaction across
models with a single publish point, and no use case in the canonical plug-ins needs
it. Record as an open question; do not build it.

### 1.4 VST3 meters

Meters ride in `data.outputParameterChanges` and the host forwards them to the
controller per-parameter at whatever rate it chooses. There is no mechanism to make
that atomic.

Mitigation, if it proves necessary: a generation counter in the
`export_param_offset` range that the controller treats as a commit barrier —
stage incoming meter values, apply the staged set when the generation arrives.
Hosts may drop or reorder, so this is best-effort and must be described that way.
**Do not build it speculatively**; measure whether visible tearing occurs first.

### 1.5 AAX segment transactions

AAX is the one format where a *state load* can tear today. `SetChunk` reaches the
algorithm through segmented coefficient packets, and each segment is an independent
`PostPacket` with its own `seq` that
[alg_proc.cpp](../wrappers/aax/source/alg_proc.cpp) diffs separately. At 10,000
params that is 667 packets which the host may land across different render calls.

Design:

- Add `uint32_t txn` to `Coef_segment` (there is room: 15 doubles + `seq` = 128
  bytes exactly, so this requires re-checking the packing budget — see
  [open question O3](#open-questions)).
- The data model stamps every segment of one logical change with the same `txn`
  and records `txn_segment_count`.
- The algorithm stages arriving segments and applies a `txn` only when all its
  segments are present.
- **Incomplete transactions:** apply after a bounded number of blocks rather than
  stalling indefinitely, and log a `latency`-style drop probe. A permanently
  stalled load is worse than a torn one.

Automation must **not** be transactional — a breakpoint is a single value with a
timestamp, and batching would defeat the host's sample-accurate delivery. Only
`SetChunk` and bulk state pushes get a `txn`; live automation keeps today's path.

### 1.6 Cheap fix to land alongside

[alg_proc.cpp:37](../wrappers/aax/source/alg_proc.cpp#L37) scans every segment each
render block to check `seq`. At 667 segments that touches ~85 KB per block purely to
ask "did anything change." Add a single global dirty counter checked first. Small,
independent, and worth doing regardless of the rest of this plan.

## Part 2: `state::Model` — synchronized non-parameter state

**Goal.** The author declares a set of POD slots; the framework synchronizes,
persists, and undoes them with no further work — exactly the promise `params::Model`
makes for scalars.

### 2.1 Declaration

Mirrors `Worker::Model`, discovered via `__has_include` like
[worker.hpp](../examples/worker_demo/source/worker.hpp), collapsing to
`std::monostate` when absent (`TINY_HAS_STATE`).

```cpp
// examples/seq_demo/source/state.hpp
namespace tiny::plugin {

struct Pattern {
    static constexpr auto version = uint32_t{1};
    struct Step {
        std::array<int8_t, 4> notes{-1, -1, -1, -1}; // -1 = empty
        uint8_t velocity{100};
        uint8_t gate{50};
        uint8_t flags{};
    };
    std::array<Step, 64> steps{};
    uint8_t length{16};
};

struct Chain_order {
    static constexpr auto version = uint32_t{1};
    std::array<uint8_t, 8> slot{};
};

class State {
public:
    struct Model {
        enum class Address : uint32_t { Pattern_a, Pattern_b, Chain, Num_slots };

        // Position-parallel to Address. The framework derives the change variant.
        using Slots = std::tuple<Pattern, Pattern, Chain_order>;

        static constexpr auto undo_depth = size_t{128};
    };
};

} // namespace tiny::plugin
```

**Declare `Slots` as a tuple, not `Types` as a variant.** One declaration instead of
two that can disagree, and the slot→type binding becomes compile-time:
`state.get<Address::Pattern_a>()` returns `const Pattern&` with no `get_if`. The
framework derives the variant internally for change records.

Framework-side static assertions:

```cpp
static_assert(std::tuple_size_v<Slots> == enum_raw(Address::Num_slots));
static_assert(all_trivially_copyable_v<Slots>);
static_assert(all_have_version_v<Slots>);
```

### 2.2 Why POD is the enabling constraint, not a concession

Every hard problem this design would otherwise face is dissolved by trivial
copyability, using machinery that **already exists**:

| Problem | Solved by |
|---|---|
| Editor↔processor transport | `Lock_free_queue` / `Transaction` — same as the worker |
| VST3's two distributable components | `send_variant`/`reconstruct_variant` in [vst3_messaging.h](../wrappers/vst3/source/vst3_messaging.h) already requires trivially-copyable alternatives — **works verbatim** |
| AAX's two-component split | Direct Data is a `Byte_ring` memcpy of a byte range |
| Undo storage | A step holds before/after by value: no serializer, no lifetime rules, no eviction policy |
| Persistence | A POD is bytes |
| RT safety | Fixed size, no allocation |

Fixed-size char arrays as POD strings are already the house idiom — the worker demo
uses `std::array<char, 64>` for a UUID and `std::array<char, 128>` for a path. This
means a **file reference is expressible as a slot** (`std::array<char, 512>` +
`uint64_t` hash), which is what makes the buffer-system merge in
[2.6](#26-relationship-to-buffer-system) work.

### 2.3 Three scoping decisions

These are what make the model well-defined rather than open-ended.

**Editor-authored only; the processor reads.** Mirrors params exactly (host/editor
write, processor reads), eliminates two-writer conflict resolution, and draws a
clean line: processor-authored data is `Meter_model` and `Block_model`. A looper
recording is therefore explicitly *not* a state slot — it is buffer-system content.

**Whole-slot replace only.** "Set slot 3 to this value." Coalescing during a drag
comes free from the existing `Action_start`/`Action_end` brackets.

**State changes do not enter `process::Event::Any`.** A 450-byte `Pattern` in the
process event variant would inflate `sizeof(Event::Any)` for *every* event and wreck
the RT event-vector sizing (see [A.4](#a4-audio-thread-costs)). State changes arrive
on their own `Transaction`, drained at the top of `process` exactly like worker
replies. They are not sample-accurate and carry no frame offset.

### 2.4 Intake and undo

`User_action` gains a `Set_state{address, payload}` alternative, so state edits ride
the same choke point that makes param undo work — and inherit gesture brackets, so a
"load kit" that swaps a pattern *and* nudges twelve params coalesces into **one undo
step** automatically.

`Undo_step` generalizes:

```cpp
struct Param_change { uint32_t addr; double from; double to; };
struct State_change  { uint32_t slot; Slot_variant from; Slot_variant to; };
using Change = std::variant<Param_change, State_change>;
struct Undo_step { std::vector<Change> changes{}; };
```

The editor reads current slot values from `Plugin_state` to supply `from`, the same
way it does for params.

**Sizing guidance is load-bearing.** A change record is
`2 × max(sizeof(alternatives))`, so one fat alternative taxes every record. Prefer
many small slots to one large one — which is also what you want for undo
granularity, so the incentives align. Document a soft ceiling (~4 KB per slot) and
assert on it. A wavetable does not go in a slot; it goes in buffer-system.

**Add a depth cap to `Undo_history` while here.** `_undo_stack` is currently an
uncapped `std::vector<Undo_step>`; at scale a preset-load step is ~240 KB (see
[A.3](#a3-memory)). `Model::undo_depth` gives the author the knob.

### 2.5 POD layout is a new permanence surface

This is the one thing genuinely harder than params, and it must be designed in from
the start rather than discovered after a plug-in ships.

A parameter's representation is a `double` forever. A slot's representation is a
struct layout, and **adding a field changes its size and silently misreads every
existing chunk.** `param-identity-and-ordering.md` documents three permanence
surfaces; this is a fourth with a worse failure mode.

Mechanism:

- Every slot type declares `static constexpr uint32_t version`.
- The chunk stores `(slot_address, version, byte_length, bytes)` per slot.
- On load: version match ⇒ memcpy; mismatch ⇒ the author's optional
  `migrate_slot<T>(uint32_t from_version, std::span<const std::byte>) -> T` hook,
  concept-detected; no hook ⇒ default-construct and log a `state` probe.
- `byte_length` mismatch at a matching version is a **hard error**, not a silent
  accept — that is the layout-drift case.

Add to the permanence documentation alongside `Address`, `identifier`, and
`au_order()`: **the `Slots` tuple order is frozen, and every slot type's layout is
frozen at its declared version.**

### 2.6 Relationship to buffer-system

**Merge the declaration and persistence; keep the transports separate.**

`Buffer_source` is a descriptor — path + sha256, or "embedded, N bytes". That is a
POD. The bytes are the payload it references.

- The **descriptor** becomes a state slot → inherits sync, transactional publish,
  undo and persistence with no new machinery.
- The **bytes** stay with buffer-system → `prepare`, atomic install, deferred
  retire, the container format, the AAX raw chunk.

Consequences for [buffer-system.md](buffer-system.md):

1. Most of its **Persistence** section deletes. It exists largely to route the
   *descriptor* through two codepaths; under the merge only the byte payload needs
   the container and raw-chunk work.
2. Its stated dependency — *"`push`/`remove` are discrete undoable actions the
   editor records in the existing `Undo_history`"* — becomes true. It is currently
   **not implementable**: `Undo_step` holds only `Param_change` and
   `process_actions` accepts only a `User_action` span. There is no representation
   for "a pad was loaded."
3. Its exclusion *"No generic non-audio blob system. Sequences/patterns are params"*
   is **reversed**, and the "Relationship to the rest of the framework" table needs
   a `state::Model` row. Do this deliberately, in that document, not by drift.

**Sequencing caveat.** This couples the September looper slice to a model that does
not exist. Recommended: design the seam now, build the looper against a hand-rolled
descriptor, migrate when `state::Model` lands. Do not block the vertical slice on
the abstraction.

## Part 3: State v2

### 3.1 What is wrong with v1

**Three different value spaces.** Verified in the code:

| Surface | Space | Width |
|---|---|---|
| VST3 session chunk | **knob** | float32 |
| CLAP session chunk | **host** | float32 |
| AUv2 / AUv3 (via AUBase `Globals`) | **host** | float32 |
| AAX chunk | **plain** | float32 |
| JSON preset (`State_adapter`) | **plain** | float32 |

For `Real` semantics host space is a *linear* 0…1 rescale of plain, while knob space
runs through the `Knob_adapter`. Therefore:

> **Storing knob space makes `Adapter::Any` an undocumented permanence surface.**
> Ship, later retune a `Taper` from 0.4 to 0.35 because it feels better, and every
> saved VST3 session silently shifts every affected value.

Robustness ranks plain > host > knob. VST3 uses the worst one.

**Two incompatible identity models.** Binary session chunks are *positional* —
`stored_values[i]` matched to `param_spec(i)`, with a count comparison as the only
migration. JSON presets are *keypath-keyed* and self-describing. Only one of them
can survive a parameter being retired.

**No schema version.** The header is `(framework, manufacturer, plugin, count)`. Any
future layout change must be inferred from byte length, and a v1 build reading a v2
chunk will misparse rather than refuse.

**Migration logic duplicated five times.** The count-mismatch branch, default
filling, the `no_value` sentinel check, the persistent-param skip — hand-rolled per
wrapper with small variations (VST3 reads the whole stream but sizes its vector by
`min`; CLAP branches differently). This is the pattern
[meter_publisher.hpp](../libs/tinyplug/include/tinyplug/meter_publisher.hpp) already
calls out: *"five hand-rolled copies is exactly how the policies drifted apart."*

**`is_persistent` reimplemented inline** in `State_adapter::add_value` as
`spec.policy != Policy::Interface` rather than calling `State_rules::is_persistent`.

### 3.2 Architecture: one model layer, two encoders, five transports

```
Declarations   params::Model    state::Model    Buffer_model    editor State_map
                        |
                        |  one gather() / one apply()
                        v
State_document   version + model_hash + entries    (canonical: PLAIN space, double)
                        |
                        |  two encoders
        +---------------+---------------+
        v                               v
  binary codec                    JSON codec (+ container for bytes)
        |                               |
        v                               v
  five thin transports          State_adapter / exporters
  VST3  CLAP  AUv2  AUv3  AAX
```

That formula is the explicit answer to "how much duplication is right":
**1 model layer, 2 encoders, 5 transports.**

This reconciles with buffer-system.md's *"Why not one serializer"* rejection, which
was correct on its own terms — that decision was about not dragging JSON/DOM cost
onto the host-session hot path, and v2 preserves both encoders. What it unifies is
the layer *above* them, which is where the 5× duplication actually lives.

### 3.3 `State_document`

```cpp
struct State_document {
    uint32_t version{2};
    uint32_t model_hash{};             // Over (address, identifier) pairs.
    std::vector<double> params{};      // Plain space, dense by address.
    State_map editor{};
    std::vector<Slot_entry> slots{};   // {address, version, bytes}
    std::vector<Buffer_entry> buffers{};
};
```

- **`gather()`** builds it from `Value_store` + `Editor::save_state()` + slots.
- **`apply()`** is the *only* place count-mismatch, default filling, persistence
  filtering and slot migration live.
- **`model_hash`** is the cheap runtime half of
  [param-lockfile.md](param-lockfile.md): a mismatch means the append-only contract
  was violated, so refuse cleanly instead of corrupting. Full lockfile enforcement
  stays deferred.

Params stay **dense-by-address** rather than keyed — `Address` is append-only by
contract, so dense is correct and compact, and `model_hash` catches the violation
case. Non-persistent params keep occupying a slot for positional stability, but the
`float`-lowest sentinel is replaced by an explicit presence bitmap.

### 3.4 Float → double

Adopt double in the canonical document and both encoders.

- JSON: free, it is text.
- Binary: 4 bytes per param — 40 KB at 10,000 params, irrelevant next to
  [A.3](#a3-memory).
- float32's 24-bit mantissa is *exactly* the ceiling on packed-`Int` parameter
  encodings, so there is currently zero headroom.
- `State_rules::no_value` as a magic float disappears.

AUv2/AUv3 hosts store `AudioUnitParameterValue` as float32 regardless — but that is
the *host's* mirror, not our chunk, and with `Value_store` canonical the chunk is
exact even when the mirror is not.

### 3.5 Bridging v1 → v2 (explicit)

1. **Version sniff.** v2 writes a distinguishable header; the loader dispatches on
   it. **Keep the v1 reader permanently** — ~40 lines per format, and the only way
   old sessions ever open again.
2. **Convert space on read.** v1 VST3 chunks are knob, CLAP/AU are host, AAX is
   plain. Each v1 reader converts to plain using the *current* adapters. This is
   lossy exactly where an adapter changed since the save — the pre-existing hazard,
   and moving to plain is what stops it recurring.
3. **JSON is nearly a superset already** (keypath-keyed, plain space). Bump
   `version`; float→double is transparent to readers; existing exporters in
   [tools/presets/](../tools/presets/) keep working.
4. **Do not write v1.** Hosts preserve chunks they do not understand, so downgrade
   safety only requires a v2 chunk to be *rejected* cleanly by an old build — which
   the version word provides and v1 lacks.
5. **Per-format migration is independent.** Behind the sniff, each wrapper moves on
   its own schedule. This is what makes it a safe refactor rather than a big bang.

### 3.6 Also fix while here

- **`Policy::Hidden` in VST3.** `kIsHidden` is commented out at
  [controller.cpp:157](../wrappers/vst3/source/controller.cpp#L157) because Studio
  One does not send editor changes to the processor for the hidden/read-only combo.
  AUv2, AUv3 and AAX all hide correctly; VST3 is the only gap. Re-test against
  current Studio One and find a flag combination that hides without breaking the
  editor→processor path.
- **Spec copy reduction.** `params::Infos` keeps three full `std::vector<Spec>`
  copies ([tiny_params.hpp:479-482](../libs/tinyplug/include/tinyplug/tiny_params.hpp#L479-L482)).
  `indexed_specs` and `au_specs` can be `std::vector<uint32_t>` permutations into
  `display_specs`. Saves ~4 MB and ~60k string copies at 10,000 params; harmless at
  50.

## Staging

Each stage is independently shippable and leaves the tree green.

| # | Work | Depends on | Size |
|---|---|---|---|
| 1 | `Transaction<T, capacity>` (F1); migrate the two `Change_list` users | — | S |
| 2 | AAX per-block dirty counter ([1.6](#16-cheap-fix-to-land-alongside)) | — | XS |
| 3 | Undo depth cap; `Undo_step` variant generalization | — | S |
| 4 | Meter snapshot publish ([1.2](#12-meters-snapshot-publish)) | 1 | M |
| 5 | `Value_store` (F2); wrappers mirror from it | — | M |
| 6 | `State_document` + `apply()`/`gather()`; JSON encoder onto it | 5 | M |
| 7 | Binary encoder + per-format transports, one format at a time | 6 | L |
| 8 | `state::Model` declaration, transport, undo, persistence | 1, 3, 6 | L |
| 9 | AAX segment transactions ([1.5](#15-aax-segment-transactions)) | 1 | L |
| 10 | buffer-system descriptor→slot migration | 8 | M |

Stages 1–4 are self-contained and deliver the meter guarantee alone. Stage 5 is the
highest-leverage single change: it is what collapses the five state paths.

**Order note.** 9 is last because it is the hardest and the only genuinely novel
piece; 10 is last because it should not block the looper slice.

## Backwards compatibility (explicit)

1. **Zero-slot plug-ins are untouched.** No `state.hpp` ⇒ `TINY_HAS_STATE` undefined
   ⇒ every branch compiled out, chunks byte-identical.
2. **Old session → new build.** Header sniff selects the v1 reader; params and
   editor state restore; slots default.
3. **New session → old build.** Version word causes clean rejection rather than
   misparse. (v1 builds currently misparse — this is a v2-only improvement and
   cannot be retrofitted.)
4. **Old `.json` preset → new build.** No `slots` key ⇒ slots default. Additive.
5. **Meter transport change is invisible** to author code: `Dsp_context::meters`
   keeps the same `std::span<float>` scratch.

## Open questions

- **O1.** Cross-model atomicity ([1.3](#13-cross-model-atomicity)) — is one editor
  frame observing meters and state from the same block ever needed? No canonical
  plug-in requires it.
- **O2.** Does VST3 meter tearing actually manifest in practice, or is the
  generation-barrier mitigation ([1.4](#14-vst3-meters)) unnecessary? Measure before
  building.
- **O3.** `Coef_segment` is exactly 128 bytes (15 doubles + `seq`) to hit the HDX
  minimum transfer size. Adding `txn` breaks that. Drop to 14 doubles per segment
  (more segments), or steal bits from `seq`? Needs the AAX field-count ceiling
  answered first.
- **O4.** What is the AAX per-algorithm field/port ceiling? Determines the real
  param limit in AAX and whether bulk state must move to Direct Data. **Verify
  against the SDK before any large-param work.**
- **O5.** AUv3's `AUParameterTree` is proxied over XPC. What is instantiation cost
  at 1k / 5k / 10k params? Cheapest experiment on this list and the most likely to
  be disqualifying.
- **O6.** Should `migrate_slot` be mandatory when `version` is bumped, or is silent
  default-construct acceptable with a probe? Mandatory is safer; it also makes
  bumping a version a chore.

## Explicitly not in scope

- Frame-atomic delivery of **editor parameter edits**. Host-owned; impossible.
  Documented as a contract limit ([1.1](#11-what-is-and-is-not-achievable)).
- Processor-authored state slots. `Meter_model` / `Block_model` own that direction.
- Variable-cardinality slots. Slot count is frozen at ship, like `Address`.
- Full `params.lock` build-time enforcement — `model_hash` is the runtime subset;
  [param-lockfile.md](param-lockfile.md) stays deferred.
- Bulk audio in slots. That is buffer-system, and the ~4 KB soft ceiling enforces
  the boundary.

## Appendix A: parameter scaling measurements

Collected while evaluating whether a sequencer can put several thousand values in
`params::Model`. Recorded here because they are the quantitative case for
`state::Model`.

### A.1 `Policy::Hidden` hides in the UI, but nothing skips enumeration

Verified: AUv2 sets neither `IsReadable` nor `IsWritable` for `Hidden`
([effect.cpp:319](../wrappers/auv2/source/effect.cpp#L319)); AUv3 returns `{}`
flags; AAX passes `automatable = false`. All three hide correctly. VST3 is the
exception ([3.6](#36-also-fix-while-here)).

But **every format still constructs the parameter regardless of policy** — AAX
builds an `AAX_CParameter` plus a coefficient slot, AUv3 builds an `AUParameter`
ObjC object, VST3 calls `addParameter`, AUv2 returns all of them from
`GetParameterList` so the host still calls `GetParameterInfo` on each. Hiding
removes UI clutter; it removes no cost.

### A.2 AAX coefficient segments — the hard wall

`num_segments = ceil(num_coefs / 15)`; each is a separate `AddDataInPort` occupying
its own pointer slot in `Alg_context`. At 10,000 params: **667 segments**.

- 667 registered ports per algorithm. The code's own comment notes "each buffered
  port costs per-render-callback overhead."
- HDX: 667 × 128-byte DMA transfers per full update.
- `Reset_state` embeds `Coef_segment coefs[num_segments]` **by value** ≈ 85 KB,
  copied into the algorithm's memory pool on every reset.
- `Alg_state::shadow` 80 KB; `shadow_seq` 5 KB.

Estimated ceiling: ~1,000–2,000 params on Native, considerably less on HDX. Past
that the coefficient transport needs replacing, and **Direct Data is the natural
home** — it is already a memcpy of a byte range carrying meters and worker traffic.

### A.3 Memory

| Item | At 10k params | Scope |
|---|---|---|
| Three `Spec` vector copies + `Node` tree | ~6 MB | per binary (shared) |
| `std::string`s across those copies | ~90k objects | per binary, startup |
| VST3 `_events` reserve | ~2 MB | **per instance** |
| AAX `Alg_state` + `Reset_state` | ~170 KB | per instance |
| AUv3 `AUParameterTree` | ~4 MB ObjC | per instance, over XPC |
| Undo step (full preset load) | ~240 KB | per step, **uncapped today** |

`sizeof(Spec)` is estimated at ~200 bytes and should be measured.

### A.4 Audio-thread costs

**VST3 event vector.** `events_size = 4 * num_params + scale * 64 *
bit_width(num_params) + 1` ([audio_effect.cpp:158](../wrappers/vst3/source/audio_effect.cpp#L158)).
At 10,000 params / 512 samples: ~41,800 events ≈ 2 MB reserved, and a state load can
push 40,000 events into one block. Sorting those is ~612k comparisons ≈ 1–2 ms.
Against 10.7 ms that is a spike; **at a 64-sample buffer (1.33 ms budget) a preset
load is a guaranteed overrun.** Stage 6's `apply()` should bound per-block state
delivery rather than pushing an entire load at once.

**`Change_list::consume`** iterates a `std::unordered_map` on the audio thread —
~10,000 pointer-chasing misses ≈ 1 ms at scale. Stage 1 removes this.

**AAX per-block segment scan** — see [1.6](#16-cheap-fix-to-land-alongside).

### A.5 Conclusion

A sequencer at several thousand parameters is workable in VST3, CLAP and AUv2 with
these fixes; marginal in AUv3; not viable in AAX under the current coefficient
design. Tens of thousands is not reachable in any format, because the wall is not in
tinyplug — **every format's contract is built around enumerating parameters**, and
that cost is per-param and unavoidable.

Which is the argument for `state::Model`: a `Pattern` POD holding 64 steps × 4 notes
is *one* slot — one thing to persist, zero host enumeration, one transactional
publish — versus 256 parameters the host must name, describe and list. Params stay
for what genuinely wants automation (step modifiers, a few hundred at most); bulk
data goes in slots.
