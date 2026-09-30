#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <tuple>
#include <thread>
#include <vector>

#include "support/sequencer.hpp"
#include <tiny_core/tiny_state.hpp>

#include "support/pipe.hpp"
#include "support/undo_log.hpp"

namespace {

using demo::Pattern;
using tiny::test::Undo_log;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

auto expect_eq(long long got, long long want, const char* what) -> void
{
    const auto ok = got == want;
    if (!ok) ++failures;
    std::printf("  [%s] %s (got %lld, want %lld)\n", ok ? "pass" : "FAIL", what, got, want);
}

struct Rig {
    explicit Rig(bool coupled) : down{coupled}, up{coupled, 64}
    {
        down.set_sink([this](std::span<const std::byte> b, uint32_t t) { proc.on_edit(b, t); });
        up.set_sink([this](std::span<const std::byte> b, uint32_t g) { editor.on_snapshot(b, g); });
    }

    tiny::state::Processor_side<Pattern> proc{};
    tiny::state::Editor_side<Pattern> editor{};
    tiny::link::Pipe down;
    tiny::link::Pipe up;
    Undo_log undo{16u * 1024 * 1024};

    auto block() -> void { auto b = proc.begin_block(); (void) b; }
    auto record(size_t tr, size_t st, uint8_t key) -> void
    {
        auto b = proc.begin_block();
        b.mutate().tracks[tr].steps[st].notes[0].key = key;
    }
    auto settle() -> void
    {
        for (auto i = 0; i < 3; ++i) {
            editor.flush(down); down.pump(); block(); proc.pump(up); up.pump();
        }
    }
    auto live() -> Pattern { auto s = Pattern{}; proc.snapshot(s); return s; }
};

// MARK: - back-to-back edits with no block between

auto test_edits_back_to_back() -> void
{
    std::printf("two editor edits with nothing consumed between them\n");

    auto rig = Rig{true}; // immediate delivery, but no block runs between the two edits

    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[0].notes[0].key = 11; });
    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[1].notes[0].key = 22; });
    rig.editor.flush(rig.down);
    rig.block();

    const auto s = rig.live();
    expect_eq(s.tracks[1].steps[0].notes[0].key, 11, "first edit survives coalescing");
    expect_eq(s.tracks[1].steps[1].notes[0].key, 22, "second edit lands");

    // Five in a row with no flush between: one message must carry all of them.
    rig.settle();
    rig.down.reset_counters();
    for (auto i = 2; i < 7; ++i) {
        rig.editor.edit([=](Pattern& p) {
            p.tracks[1].steps[static_cast<size_t>(i)].notes[0].key = static_cast<uint8_t>(10 * i);
        });
    }
    rig.editor.flush(rig.down);
    rig.block();
    const auto s2 = rig.live();
    auto lost = 0;
    for (auto i = 2; i < 7; ++i) {
        if (s2.tracks[1].steps[static_cast<size_t>(i)].notes[0].key != 10 * i) ++lost;
    }
    expect_eq(lost, 0, "five coalesced edits all land");
    expect_eq(static_cast<long long>(rig.down.sent_msgs()), 1, "and cost exactly one message");
}

// MARK: - back-to-back processor updates with no snapshot between

auto test_updates_back_to_back() -> void
{
    std::printf("\ntwo processor updates with nothing pumped between them\n");

    auto rig = Rig{false};

    rig.record(0, 0, 7);
    rig.record(0, 1, 8);
    rig.proc.pump(rig.up);
    rig.up.pump();

    expect_eq(rig.editor.view().tracks[0].steps[0].notes[0].key, 7, "first update visible");
    expect_eq(rig.editor.view().tracks[0].steps[1].notes[0].key, 8, "second update visible");

    // Two snapshots queued, delivered in order.
    rig.record(0, 2, 9);
    rig.proc.pump(rig.up);
    rig.record(0, 3, 10);
    rig.proc.pump(rig.up);
    expect_eq(static_cast<long long>(rig.up.pending()), 2, "two snapshots queued");
    rig.up.pump();
    expect_eq(rig.editor.view().tracks[0].steps[3].notes[0].key, 10, "editor converges on the newest");
}

// MARK: - out-of-order snapshot delivery

auto test_out_of_order_snapshots() -> void
{
    std::printf("\nsnapshots delivered out of order (IMessage guarantees nothing)\n");

    auto proc = tiny::state::Processor_side<Pattern>{};
    auto editor = tiny::state::Editor_side<Pattern>{};

    // Capture two snapshots by hand so we can deliver them backwards. The payload is
    // an Ack followed by the value, so build it the way pump() does.
    const auto wrap = [](const Pattern& p) {
        auto buf = std::vector<std::byte>(sizeof(tiny::state::Ack) + sizeof(Pattern));
        const auto ack = tiny::state::Ack{};
        std::memcpy(buf.data(), &ack, sizeof(ack));
        std::memcpy(buf.data() + sizeof(ack), &p, sizeof(Pattern));
        return buf;
    };

    { auto b = proc.begin_block(); b.mutate().tracks[0].steps[0].notes[0].key = 1; }
    auto s1 = Pattern{}; proc.snapshot(s1);
    const auto g1 = proc.edits();

    { auto b = proc.begin_block(); b.mutate().tracks[0].steps[1].notes[0].key = 2; }
    auto s2 = Pattern{}; proc.snapshot(s2);
    const auto g2 = proc.edits();

    expect(g2 > g1, "generations are monotonic");

    editor.on_snapshot(wrap(s2), g2);
    editor.on_snapshot(wrap(s1), g1); // late arrival

    expect_eq(editor.view().tracks[0].steps[1].notes[0].key, 2,
              "a stale snapshot must not roll the view back");
    expect_eq(static_cast<long long>(editor.generation()), static_cast<long long>(g2),
              "generation does not regress");
}

// MARK: - ABA

auto test_aba() -> void
{
    std::printf("\nABA\n");

    auto rig = Rig{true};

    // Value goes 0 -> 5 -> 0 across two commits: two steps, each exact.
    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 5; });
    rig.settle();
    rig.editor.commit(rig.undo);
    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 0; });
    rig.settle();
    rig.editor.commit(rig.undo);
    expect_eq(static_cast<long long>(rig.undo.steps()), 2, "A->B->A across two commits is two steps");

    const auto on_param = [](uint32_t, double) {};
    const auto on_state = [&](const std::byte* b, const std::byte* n, size_t sz) {
        rig.editor.apply_replay(b, n, sz);
    };

    rig.undo.undo(on_param, on_state); rig.settle();
    expect_eq(rig.live().tracks[0].steps[0].notes[0].key, 5, "undo of B->A gives B");
    rig.undo.undo(on_param, on_state); rig.settle();
    expect_eq(rig.live().tracks[0].steps[0].notes[0].key, 0, "undo of A->B gives A");

    // Same round trip inside ONE commit window: net zero, so no step at all.
    auto rig2 = Rig{true};
    rig2.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 5; });
    rig2.settle();
    rig2.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 0; });
    rig2.settle();
    rig2.editor.commit(rig2.undo);
    expect_eq(static_cast<long long>(rig2.undo.steps()), 0, "A->B->A inside one window is no step");
}

// MARK: - undo / redo interleaving

auto test_undo_interleaving() -> void
{
    std::printf("\nundo / redo interleaving\n");

    auto rig = Rig{true};
    const auto on_param = [](uint32_t, double) {};
    const auto on_state = [&](const std::byte* b, const std::byte* n, size_t sz) {
        rig.editor.apply_replay(b, n, sz);
    };

    for (auto i = 1; i <= 3; ++i) {
        rig.editor.edit([=](Pattern& p) {
            p.tracks[0].steps[static_cast<size_t>(i)].notes[0].key = static_cast<uint8_t>(i);
        });
        rig.settle();
        rig.editor.commit(rig.undo);
    }
    expect_eq(static_cast<long long>(rig.undo.steps()), 3, "three steps");

    rig.undo.undo(on_param, on_state); rig.settle();
    rig.undo.undo(on_param, on_state); rig.settle();
    expect_eq(rig.live().tracks[0].steps[2].notes[0].key, 0, "two undos");
    expect_eq(rig.live().tracks[0].steps[1].notes[0].key, 1, "not three");
    expect_eq(static_cast<long long>(rig.undo.redo_steps()), 2, "two on the redo stack");

    rig.undo.redo(on_param, on_state); rig.settle();
    expect_eq(rig.live().tracks[0].steps[2].notes[0].key, 2, "redo restores");

    // A new edit must invalidate the redo stack.
    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[9].notes[0].key = 99; });
    rig.settle();
    rig.editor.commit(rig.undo);
    expect_eq(static_cast<long long>(rig.undo.redo_steps()), 0, "a new edit clears redo");
    expect(!rig.undo.redo(on_param, on_state), "redo now refuses");
}

// MARK: - commit gating

auto test_commit_gating() -> void
{
    std::printf("\ncommit while an edit is in flight\n");

    auto rig = Rig{false}; // queued: the edit sits on the wire

    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 3; });
    expect(rig.editor.in_flight(), "edit is outstanding");
    expect(!rig.editor.commit(rig.undo), "commit refuses while outstanding");
    expect_eq(static_cast<long long>(rig.undo.steps()), 0, "no step recorded");

    rig.settle();
    expect(!rig.editor.in_flight(), "reconciled after the round trip");
    expect(rig.editor.commit(rig.undo), "commit now succeeds");
    expect_eq(static_cast<long long>(rig.undo.steps()), 1, "exactly one step");
}

// MARK: - randomized interleaving, disjoint regions

// Draws are `rng() % n`, never a <random> distribution: those are implementation-defined, so
// libc++ and MSVC would replay different histories from one seed. A single seed hid a lost edit
// for as long as the one libc++ history happened to pass; hence several.
constexpr auto fuzz_seeds = 16u;

auto run_fuzz(uint32_t seed) -> std::tuple<int, int, size_t, int, bool>
{
    auto rng = std::mt19937{seed};

    auto rig = Rig{false};
    auto proc_writes = std::vector<uint8_t>(64, 0);
    auto edit_writes = std::vector<int16_t>(64, 0);
    auto n_edits = 0;

    for (auto i = 0; i < 20000; ++i) {
        switch (rng() % 6) {
            case 0: {
                const auto st = static_cast<size_t>(rng() % 64);
                const auto key = static_cast<uint8_t>(1 + rng() % 120);
                rig.record(0, st, key);
                proc_writes[st] = key;
                break;
            }
            case 1: {
                const auto st = static_cast<size_t>(rng() % 64);
                const auto v = static_cast<int16_t>(1 + rng() % 30000);
                rig.editor.edit([=](Pattern& p) { p.tracks[1].steps[st].mods[0].value = v; });
                edit_writes[st] = v;
                ++n_edits;
                break;
            }
            case 2: rig.editor.flush(rig.down); rig.down.pump(1); break;
            case 3: rig.block(); break;
            case 4: rig.proc.pump(rig.up); break;
            case 5: rig.up.pump(1); break;
        }
    }
    for (auto i = 0; i < 8; ++i) rig.settle();

    const auto s = rig.live();
    auto lost_proc = 0;
    auto lost_edit = 0;
    for (auto st = size_t{}; st < 64; ++st) {
        if (proc_writes[st] != 0 && s.tracks[0].steps[st].notes[0].key != proc_writes[st]) ++lost_proc;
        if (edit_writes[st] != 0 && s.tracks[1].steps[st].mods[0].value != edit_writes[st]) ++lost_edit;
    }
    const auto converged = std::memcmp(&rig.editor.view(), &s, sizeof(Pattern)) == 0;
    return {lost_proc, lost_edit, rig.proc.conflicts(), n_edits, converged};
}

auto test_fuzz_interleaving() -> void
{
    std::printf("\nrandomized interleaving, disjoint writers (20k ops x %u seeds)\n", fuzz_seeds);

    auto lp = 0, le = 0, n_edits = 0;
    auto conflicts = size_t{};
    auto converged = true;
    for (auto seed = 1u; seed <= fuzz_seeds; ++seed) {
        const auto [p, e, c, n, conv] = run_fuzz(seed);
        lp += p;
        le += e;
        conflicts += c;
        n_edits += n;
        converged = converged && conv;
    }
    std::printf("  ....  %d lost proc, %d lost edit, %zu conflicting bytes over %d edits\n",
                lp, le, conflicts, n_edits);

    expect_eq(lp, 0, "every processor value present");
    expect_eq(le, 0, "every editor value present");
    expect(converged, "editor view is byte-identical to processor state after draining");
    // Conflicts are advisory. A handful survive adversarial pump ordering; the point is
    // that the counter still signals, rather than being buried under thousands.
    expect(conflicts * 200 < sizeof(Pattern) * static_cast<size_t>(n_edits),
           "conflict counter stays a usable signal (<0.5% of bytes sent)");
}

// MARK: - the host suspends the processor

auto run_suspended() -> void
{
    std::printf("\nhost suspends the processor\n");

    auto rig = Rig{false};

    // Normal operation first.
    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 1; });
    rig.settle();
    rig.editor.commit(rig.undo);
    expect_eq(static_cast<long long>(rig.undo.steps()), 1, "one step before suspension");

    // Host stops calling process. Messages still flow; nothing is ever applied.
    for (auto i = 1; i < 20; ++i) {
        rig.editor.edit([=](Pattern& p) {
            p.tracks[0].steps[static_cast<size_t>(i)].notes[0].key = static_cast<uint8_t>(i + 1);
        });
        rig.editor.flush(rig.down);
        rig.down.pump();
        rig.proc.pump(rig.up);
        rig.up.pump();
        rig.editor.commit(rig.undo);
    }

    std::printf("  ....  %zu undo steps, editor view intact: %s\n",
                rig.undo.steps(),
                rig.editor.view().tracks[0].steps[19].notes[0].key == 20 ? "yes" : "NO");

    expect(rig.editor.view().tracks[0].steps[19].notes[0].key == 20,
           "the editor never loses an edit while suspended");

    // What would be PERSISTED if the session were saved right now.
    auto persisted = Pattern{};
    rig.proc.snapshot(persisted);
    auto in_chunk = 0;
    for (auto i = 1; i < 20; ++i) {
        if (persisted.tracks[0].steps[static_cast<size_t>(i)].notes[0].key == i + 1) ++in_chunk;
    }
    std::printf("  ....  %d of 19 suspended edits would reach the processor's chunk\n", in_chunk);
    expect_eq(in_chunk, 19, "a save while suspended still contains every edit");

    // Host resumes.
    for (auto i = 0; i < 8; ++i) rig.settle();
    auto landed = 0;
    const auto s = rig.live();
    for (auto i = 1; i < 20; ++i) {
        if (s.tracks[0].steps[static_cast<size_t>(i)].notes[0].key == i + 1) ++landed;
    }
    expect_eq(landed, 19, "everything lands once the host resumes");

    rig.editor.commit(rig.undo);
    std::printf("  ....  %zu undo steps after resume\n", rig.undo.steps());
}

// The case that actually matters: the host stops calling process and never says so.
auto test_stalled_silently() -> void
{
    std::printf("\nhost stalls process WITHOUT deactivating\n");

    auto rig = Rig{false};

    for (auto i = 0; i < 20; ++i) {
        rig.editor.edit([=](Pattern& p) {
            p.tracks[0].steps[static_cast<size_t>(i)].notes[0].key = static_cast<uint8_t>(i + 1);
        });
        rig.editor.flush(rig.down);
        rig.down.pump();
        rig.proc.pump(rig.up);
        rig.up.pump();
    }

    auto persisted = Pattern{};
    rig.proc.snapshot(persisted);
    auto in_chunk = 0;
    for (auto i = 0; i < 20; ++i) {
        if (persisted.tracks[0].steps[static_cast<size_t>(i)].notes[0].key == i + 1) ++in_chunk;
    }
    std::printf("  ....  %d of 20 edits reach a save taken mid-stall\n", in_chunk);
    expect_eq(in_chunk, 20, "a save mid-stall sees every edit, from the PROCESSOR's chunk");
    expect(rig.editor.view().tracks[0].steps[19].notes[0].key == 20,
           "the editor's view is complete too");

    for (auto i = 0; i < 8; ++i) rig.settle();
    auto landed = 0;
    const auto s = rig.live();
    for (auto i = 0; i < 20; ++i) {
        if (s.tracks[0].steps[static_cast<size_t>(i)].notes[0].key == i + 1) ++landed;
    }
    expect_eq(landed, 20, "and nothing is lost once it resumes");
}

auto test_suspended() -> void
{
    run_suspended();
    test_stalled_silently();
}

// MARK: - contested bytes

auto test_contested_byte() -> void
{
    std::printf("\nboth sides writing the SAME byte\n");

    auto rig = Rig{false};
    auto editor_won = 0;
    auto processor_won = 0;

    for (auto round = 0; round < 200; ++round) {
        // Interleave the two writers over one byte with varying phase.
        rig.editor.edit([=](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 111; });
        if (round % 2) { rig.editor.flush(rig.down); rig.down.pump(); }
        rig.record(0, 0, 222);
        if (round % 3) rig.proc.pump(rig.up);
        rig.editor.flush(rig.down);
        rig.down.pump();
        rig.block();
        rig.proc.pump(rig.up);
        rig.up.pump();

        for (auto i = 0; i < 4; ++i) rig.settle();


        const auto now = rig.live();
        const auto live = now.tracks[0].steps[0].notes[0].key;
        if (live == 111) ++editor_won; else if (live == 222) ++processor_won;

        if (std::memcmp(&rig.editor.view(), &now, sizeof(Pattern)) != 0) {
            expect(false, "contested byte still converged");
            return;
        }

        rig.editor.edit([=](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 0; });
        for (auto i = 0; i < 4; ++i) rig.settle();
    }

    std::printf("  ....  editor won %d, processor won %d of 200 rounds\n", editor_won, processor_won);
    expect(true, "always converged, both sides ending byte-identical");
    expect(editor_won + processor_won == 200, "the value is always one of the two written");
}

// MARK: - triple buffer monotonicity under load

auto test_monotonic_reads() -> void
{
    std::printf("\ntriple buffer: reads never go backwards\n");

    struct Counter { uint64_t n{}; uint64_t pad[7]{}; };

    auto store = tiny::state::Store<Counter>{};
    auto stop = std::atomic<bool>{false};
    auto regressions = std::atomic<int>{0};
    auto reads = std::atomic<long long>{0};

    auto writer = std::thread([&] {
        for (auto i = uint64_t{1}; i <= 200000; ++i) {
            auto a = store.access();
            a.mutate().n = i;
        }
        stop.store(true);
    });

    auto last = uint64_t{};
    auto out = Counter{};
    while (!stop.load()) {
        store.save(out);
        if (out.n < last) regressions.fetch_add(1);
        last = out.n;
        reads.fetch_add(1);
    }
    writer.join();

    store.save(out);
    std::printf("  ....  %lld reads, final %llu\n",
                reads.load(), static_cast<unsigned long long>(out.n));
    expect_eq(regressions.load(), 0, "no read observes an older value than a previous read");
    expect(out.n == 200000, "final read sees the last write");
}

} // namespace

auto main() -> int
{
    test_edits_back_to_back();
    test_updates_back_to_back();
    test_out_of_order_snapshots();
    test_aba();
    test_undo_interleaving();
    test_commit_gating();
    test_fuzz_interleaving();
    test_suspended();
    test_contested_byte();
    test_monotonic_reads();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
