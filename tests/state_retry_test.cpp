#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <utility>
#include <vector>

#include "support/sequencer.hpp"
#include <tiny_core/tiny_state.hpp>

#include "support/pipe.hpp"
#include "support/undo_log.hpp"

namespace {

using demo::Pattern;
using tiny::state::Apply;

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
    tiny::state::Processor_side<Pattern> proc;
    tiny::state::Editor_side<Pattern> editor;
    tiny::link::Pipe down;
    tiny::link::Pipe up;
    tiny::test::Undo_log undo{};

    Apply policy{Apply::Retry};

    explicit Rig(Apply apply, bool immediate = false)
        : proc{}, down{immediate}, up{immediate}, policy{apply}
    {
        down.set_sink([this](std::span<const std::byte> b, uint32_t t) { proc.on_edit(b, t); });
        up.set_sink([this](std::span<const std::byte> b, uint32_t t) { editor.on_snapshot(b, t); });
    }

    // What the recorder does inside every process call, if anything.
    std::function<void(Pattern&)> recorder{};

    auto block() -> void
    {
        auto b = proc.begin_block();
        if (recorder) recorder(b.mutate());
    }

    template<typename F>
    auto record(F&& f) -> void { auto b = proc.begin_block(); f(b.mutate()); }

    // A 16 ms shuttle tick spans about six blocks at 128/48k.
    //
    // Where the staleness actually comes from is worth being exact about, because it
    // decides the refusal rate. A patch is applied at the TOP of a block, before the
    // kernel runs, so a recorder writing in the same block can never refuse it. The
    // window is entirely on the return path: the shuttle samples the buffer, the host
    // carries the message, and the processor keeps running the whole time. By the time
    // the editor draws that snapshot and the user edits against it, it is already
    // several blocks old -- and that is the gap a base goes stale in.
    static constexpr auto blocks_per_tick = 6;

    auto tick() -> void
    {
        editor.flush(down);
        down.pump();                                        // host delivers the edit
        for (auto i = 0; i < blocks_per_tick / 2; ++i) block();
        proc.pump(up);                                      // shuttle samples the buffer
        for (auto i = 0; i < blocks_per_tick / 2; ++i) block();  // still running, in transit
        up.pump();                                          // editor sees a stale snapshot
    }

    auto settle(int n = 8) -> void
    {
        auto keep = recorder;
        recorder = nullptr;
        for (auto i = 0; i < n; ++i) tick();
        recorder = keep;
    }

    auto live() -> Pattern { auto p = Pattern{}; proc.snapshot(p); return p; }
};

auto key(const Pattern& p, int t = 0, int s = 0) -> int { return p.tracks[t].steps[s].notes[0].key; }

auto seeded(Rig& rig) -> void
{
    auto seed = Pattern{};
    seed.tracks[0].steps[0].notes[0] = {60, 100};
    rig.proc.on_session_load(seed);
    rig.editor.seed(seed);
    rig.block();
}

// The gesture, written exactly as the author would. It reads everything from its T&,
// which is what makes re-running it against a different base meaningful.
const auto transpose_track_0 = [](Pattern& p) {
    for (auto& st : p.tracks[0].steps)
        for (auto& n : st.notes)
            if (n.key != 0) n.key = static_cast<uint8_t>(std::clamp(int(n.key) + 1, 1, 127));
};

// MARK: - the 61/49 case

auto test_stale_read_modify_write() -> void
{
    std::printf("read-modify-write over a region the processor is writing\n");

    auto merged = 0;
    {
        auto rig = Rig{Apply::Merge};
        seeded(rig);
        rig.editor.edit(transpose_track_0, rig.policy);
        rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0] = {48, 100}; });
        rig.settle();
        merged = key(rig.live());
        std::printf("  ....  Apply::Merge  recorded 48, transposed +1 -> %d\n", merged);
    }

    auto rig = Rig{Apply::Retry};
    seeded(rig);
    rig.editor.edit(transpose_track_0, rig.policy);
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0] = {48, 100}; });
    rig.settle();
    std::printf("  ....  Apply::Retry    recorded 48, transposed +1 -> %d  (%zu retr%s)\n",
                key(rig.live()), rig.editor.retries(), rig.editor.retries() == 1 ? "y" : "ies");

    expect_eq(merged, 61, "merge applies the stale result, as documented");
    expect_eq(key(rig.live()), 49, "CAS refuses it, the gesture re-runs, the intent composes");
    expect_eq(key(rig.editor.view()), 49, "and the editor's view agrees");
    expect_eq(static_cast<long long>(rig.proc.rejections()), 1, "exactly one refusal");
    expect_eq(static_cast<long long>(rig.editor.retries()), 1, "and exactly one retry");
}

// MARK: - the policy is a property of the gesture

// The same concurrent situation, three declarations. The processor transposes-over a
// note the editor is about to transpose (contested) and writes an unrelated track
// (not contested). What each policy does with the two is the whole distinction.
auto test_policy_per_edit() -> void
{
    std::printf("\nthe same gesture under all three policies\n");

    const auto run = [](Apply apply) {
        auto rig = Rig{Apply::Retry};
        seeded(rig);
        rig.editor.edit(transpose_track_0, apply);
        rig.record([](Pattern& p) {
            p.tracks[0].steps[0].notes[0] = {48, 100};   // contested with the transpose
            p.tracks[3].steps[0].notes[0] = {99, 100};   // nothing to do with it
        });
        rig.settle();
        const auto s = rig.live();
        return std::pair{key(s, 0, 0), key(s, 3, 0)};
    };

    const auto [ov_c, ov_u] = run(Apply::Overwrite);
    const auto [me_c, me_u] = run(Apply::Merge);
    const auto [re_c, re_u] = run(Apply::Retry);

    std::printf("  ....  %-10s contested %3d   unrelated %3d\n", "Overwrite", ov_c, ov_u);
    std::printf("  ....  %-10s contested %3d   unrelated %3d\n", "Merge",     me_c, me_u);
    std::printf("  ....  %-10s contested %3d   unrelated %3d\n", "Retry",     re_c, re_u);

    expect_eq(ov_u, 0,  "Overwrite takes the whole object, so the unrelated write is gone");
    expect_eq(me_u, 99, "Merge leaves everything outside the patch alone");
    expect_eq(re_u, 99, "so does Retry");

    expect_eq(me_c, 61, "Merge writes the result computed from a stale note");
    expect_eq(re_c, 49, "Retry re-runs the gesture and composes with it");
}

// MARK: - the editor replacing the document

// Apply::Overwrite is how the EDITOR replaces the document, which is a different
// operation from on_session_load(): that one takes bytes from outside, reseeds both
// halves and is not a gesture. This one is a gesture -- it is undoable, it sequences
// with whatever else is in flight, and it goes down the same wire as every other edit.
// The dice, INIT, "load this pattern into the rack".
auto test_editor_replaces_the_document() -> void
{
    std::printf("\nthe editor replacing the whole document\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);
    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[4].notes[0] = {70, 100}; }, Apply::Merge);
    rig.settle();
    rig.editor.commit(rig.undo);

    const auto before = rig.live();
    expect_eq(key(before, 0, 0), 60, "the seeded note");
    expect_eq(key(before, 1, 4), 70, "and an edit on top of it");

    // The dice: a whole new document, authored in a lambda like any other gesture.
    rig.editor.edit([](Pattern& p) {
        p = Pattern{};
        for (auto t = 0; t < 8; ++t) p.tracks[t].steps[0].notes[0] = {uint8_t(36 + t), 100};
    }, Apply::Overwrite);
    rig.settle();

    const auto rolled = rig.live();
    expect_eq(key(rolled, 0, 0), 36, "the new document is in");
    expect_eq(key(rolled, 1, 4), 0, "and the old one is gone, not merged with");

    // The part that makes it an edit rather than a load.
    expect(rig.editor.commit(rig.undo), "the roll closes out as an undo step");
    expect_eq(static_cast<long long>(rig.undo.steps()), 2, "two steps: the edit, then the roll");

    const auto on_param = [](uint32_t, double) {};
    const auto on_state = [&](const std::byte* b, const std::byte* n, size_t sz) {
        rig.editor.apply_replay(b, n, sz);
    };
    rig.undo.undo(on_param, on_state);
    rig.settle();

    const auto reverted = rig.live();
    expect_eq(key(reverted, 0, 0), 60, "undo brings the whole previous document back");
    expect_eq(key(reverted, 1, 4), 70, "including the edit that was on top of it");
}

// MARK: - coalescing across policies

// One message can carry several gestures but only one policy. A skipped check is not
// recoverable, so the strictest wins.
auto test_coalescing_takes_the_stricter() -> void
{
    std::printf("\ncoalescing a Merge gesture with a Retry gesture\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    // Two gestures, no flush between them, so they travel as one patch. Only the
    // second cares about drift -- and the processor drifts under both.
    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[0].notes[0] = {50, 100}; }, Apply::Merge);
    rig.editor.edit(transpose_track_0, Apply::Retry);
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0] = {48, 100}; });
    rig.settle();

    expect(rig.proc.rejections() >= 1, "the coalesced patch was checked, so Retry won");
    expect_eq(key(rig.live(), 0, 0), 49, "the read-modify-write still composed");
    expect_eq(key(rig.live(), 1, 0), 50, "and the Merge gesture rode along with it");
}

// MARK: - a refusal never loses the edit

auto test_refusal_is_not_a_loss() -> void
{
    std::printf("\na refused edit still lands\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 111; });
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 222; }); // base goes stale
    rig.editor.flush(rig.down);
    rig.down.pump();
    rig.block();                                                            // apply -> refuse

    expect_eq(static_cast<long long>(rig.proc.rejections()), 1, "the patch was refused");
    expect_eq(key(rig.live()), 222, "nothing was written over the processor's value");
    expect(rig.editor.in_flight(), "the editor still holds the gesture");

    rig.settle();
    expect_eq(key(rig.live()), 111, "the retry lands");
    expect(!rig.editor.in_flight(), "and retires");
    expect(std::memcmp(&rig.editor.view(), [&]{ static Pattern l; l = rig.live(); return &l; }(),
                       sizeof(Pattern)) == 0, "editor and processor converged");
}

// A patch the editor diffs before it hears of a refusal assumed the refused one landed, so it
// carries none of its bytes. Were it applied, the next snapshot's `applied` would pass the refusal
// and retire the refused gesture as landed: lost, with both sides agreeing. So the processor
// refuses it too (Edit_header::seen_rejected), and both are retried.
auto test_refusal_coalesced_with_a_later_apply() -> void
{
    std::printf("\na patch diffed before the editor heard of a refusal\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 111; });
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 222; }); // base goes stale
    rig.editor.flush(rig.down);
    rig.down.pump();
    rig.block();                                                            // refused

    // Before any snapshot reaches the editor: another edit, elsewhere, which lands.
    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[0].notes[0].key = 33; });
    rig.editor.flush(rig.down);
    rig.down.pump();
    rig.block();                                                            // refused too

    expect_eq(key(rig.live(), 1), 0, "the stale patch was not applied past the refusal");
    rig.proc.pump(rig.up); // One snapshot reporting both.
    rig.up.pump();

    expect(rig.editor.in_flight(), "the refused gesture is still held after that snapshot");
    rig.settle();
    expect_eq(key(rig.live()), 111, "the refused edit is retried and lands");
    expect_eq(key(rig.live(), 1), 33, "the later edit lands too");
    expect(!rig.editor.in_flight(), "and both retire");
}

// The same hole through the store: two patches that arrive before a block are coalesced into one
// staged patch named by the later sequence, so a refusal of the pair reports only that one. The
// earlier gesture must still be retried, and a patch diffed before the editor heard must not
// carry `applied` past it.
auto test_refusal_of_a_coalesced_pair() -> void
{
    std::printf("\na refusal of two coalesced patches, then a stale one\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[5].notes[0].key = 55; });
    rig.editor.flush(rig.down);                                             // seq 1
    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 111; });
    rig.editor.flush(rig.down);                                             // seq 2
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 222; }); // the pair's base goes stale
    rig.down.pump();                                                        // both staged: one patch, seq 2
    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[6].notes[0].key = 66; });
    rig.editor.flush(rig.down);                                             // seq 3, diffed before hearing
    rig.block();                                                            // the pair refused as seq 2
    rig.down.pump();
    rig.block();                                                            // seq 3 refused as stale

    const auto mid = rig.live();
    expect(key(mid, 1, 5) == 0 && key(mid, 1, 6) == 0, "neither the pair nor the stale patch applied");
    rig.proc.pump(rig.up);
    rig.up.pump();
    rig.settle();
    expect_eq(key(rig.live(), 1, 5), 55, "the first of the pair lands on retry");
    expect_eq(key(rig.live()), 111, "the second of the pair lands on retry");
    expect_eq(key(rig.live(), 1, 6), 66, "the stale patch's gesture lands on retry");
    expect(!rig.editor.in_flight(), "and all retire");
}

// A stale patch is still staged when the editor, having heard of the refusal, sends a fresh one.
// The fresh one carries every re-armed gesture, the stale one's included, so it replaces what is
// staged. Coalesced instead, the pair would be judged stale and dropped, and nothing would re-arm
// the fresh gestures: they would retire as landed without ever being applied.
auto test_fresh_patch_over_a_staged_stale_one() -> void
{
    std::printf("\na fresh patch arriving while a stale one is staged\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 111; });
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 222; }); // base goes stale
    rig.editor.flush(rig.down);
    rig.down.pump();
    rig.block();                                                            // refused: seq 1

    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[6].notes[0].key = 66; });
    rig.editor.flush(rig.down);
    rig.down.pump();                                                        // seq 2, stale, staged
    rig.proc.pump(rig.up);
    rig.up.pump();                                                          // the editor hears of seq 1
    rig.editor.flush(rig.down);
    rig.down.pump();                                                        // seq 3 lands on the staged seq 2
    rig.block();

    expect_eq(key(rig.live()), 111, "the fresh patch applies, carrying the refused gesture");
    expect_eq(key(rig.live(), 1, 6), 66, "and the stale one's");
    rig.settle();
    expect(!rig.editor.in_flight(), "and everything retires");
}

// A drag through a conflict, with the round trip several flushes long (VST3's snapshot relay, AAX
// Direct Data). Every patch in flight when the conflict is refused is stale and refused too; those
// refusals must not count as new ones the editor has to hear of, or each would make the next patch
// stale in turn and nothing would land until the drag stopped.
auto test_drag_through_a_refusal() -> void
{
    std::printf("\na continuous drag through a refusal, round trip of several flushes\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    constexpr auto lag = size_t{3}; // Messages in flight each way before the host delivers one.
    constexpr auto frames = 60;
    auto value_at = [](int frame) { return static_cast<uint8_t>(1 + frame % 120); };
    auto frame_landed = -1; // The drag frame whose value the processor last held.

    for (auto frame = 0; frame < frames; ++frame) {
        rig.editor.edit([v = value_at(frame)](Pattern& p) { p.tracks[0].steps[0].notes[0].key = v; });
        rig.editor.flush(rig.down);
        if (rig.down.pending() > lag) rig.down.pump(1);
        if (frame == 5) rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 127; }); // The conflict.
        else rig.block();
        rig.proc.pump(rig.up);
        if (rig.up.pending() > lag) rig.up.pump(1);

        const auto live = key(rig.live());
        for (auto f = frame; f >= 0 && f > frame - 20; --f) {
            if (live == value_at(f)) { frame_landed = f; break; }
        }
    }

    std::printf("  ....  %zu refusals, %zu retries; the processor last followed frame %d of %d\n",
                rig.proc.rejections(), rig.editor.retries(), frame_landed, frames - 1);
    expect(frame_landed >= frames - 1 - 4 * static_cast<int>(lag), "the processor follows the drag while it continues");
    expect(rig.editor.retries() <= 2, "one conflict costs a retry or two, not one per flush");

    rig.settle();
    expect_eq(key(rig.live()), value_at(frames - 1), "the drag's last value lands");
    expect(!rig.editor.in_flight(), "and everything retires");
}

// MARK: - how rare is rare

// The predicate is scoped to the patch footprint, so only a literal byte overlap can
// refuse. This measures the rate under a recorder running 16ths at 120 BPM while the
// editor works in three different places.
auto test_retry_rate() -> void
{
    std::printf("\nrefusal rate against a live recorder (300 edits each)\n");

    const auto run = [](const char* what, int edit_track, int edit_step_of) {
        auto rig = Rig{Apply::Retry};
        auto rec_step = 0;
        rig.recorder = [&rec_step](Pattern& p) {
            p.tracks[0].steps[static_cast<size_t>(rec_step % 64)].notes[0] =
                {static_cast<uint8_t>(40 + rec_step % 8), 100};
            ++rec_step;
        };

        for (auto i = 0; i < 300; ++i) {
            const auto st = edit_step_of < 0 ? (i % 64) : edit_step_of;
            rig.editor.edit([edit_track, st, i](Pattern& p) {
                p.tracks[edit_track].steps[static_cast<size_t>(st)].notes[0] =
                    {static_cast<uint8_t>(1 + i % 120), 100};
            });
            rig.tick();
        }
        rig.settle(64);

        std::printf("  ....  %-34s %3zu refusals / 300 edits, %zu retries\n",
                    what, rig.proc.rejections(), rig.editor.retries());
        expect(!rig.editor.in_flight(), "everything retired");
        return rig.proc.rejections();
    };

    const auto disjoint_track = run("editor on track 1, rec on 0", 1, -1);
    const auto same_track     = run("editor on track 0, rolling step", 0, -1);
    const auto same_byte      = run("editor on track 0 step 0 only", 0, 0);

    expect_eq(static_cast<long long>(disjoint_track), 0, "a disjoint region never refuses");
    expect(same_track * 10 < 300, "sharing a track but not a step stays under 10%");
    expect(same_byte > 0, "hammering the byte the recorder is writing does refuse");
}

// MARK: - undo through a refusal

auto test_undo_through_refusal() -> void
{
    std::printf("\nundo replay is itself retryable\n");

    auto rig = Rig{Apply::Retry};
    seeded(rig);

    rig.editor.edit([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 90; });
    rig.settle();
    rig.editor.commit(rig.undo);
    expect_eq(static_cast<long long>(rig.undo.steps()), 1, "one step recorded");

    // Undo, and have the processor touch the same byte before it lands.
    const auto on_param = [](uint32_t, double) {};
    const auto on_state = [&](const std::byte* b, const std::byte* n, size_t sz) {
        rig.editor.apply_replay(b, n, sz);
    };
    rig.undo.undo(on_param, on_state);
    rig.record([](Pattern& p) { p.tracks[0].steps[0].notes[0].key = 77; }); // base goes stale
    rig.editor.flush(rig.down);
    rig.down.pump();
    rig.block();                                                           // apply -> refuse
    rig.settle();

    std::printf("  ....  %zu refusals, %zu retries, final key %d\n",
                rig.proc.rejections(), rig.editor.retries(), key(rig.live()));
    expect(rig.proc.rejections() >= 1, "the undo patch was refused");
    expect_eq(key(rig.live()), 60, "and still reverted to the pre-edit value");
}

// MARK: - fuzz

// Portable draws and several seeds, for the reason given in state_stress_test.cpp.
struct Fuzz_result {
    int lost_proc{}, lost_edit{}, edits{};
    std::size_t refusals{};
    bool converged{true};
};

auto run_fuzz(uint32_t seed) -> Fuzz_result
{
    auto rng = std::mt19937{seed};

    auto rig = Rig{Apply::Retry};
    auto proc_writes = std::vector<uint8_t>(64, 0);
    auto edit_writes = std::vector<int16_t>(64, 0);
    auto r = Fuzz_result{};

    for (auto i = 0; i < 20000; ++i) {
        switch (rng() % 6) {
            case 0: {
                const auto st = static_cast<size_t>(rng() % 64);
                const auto k = static_cast<uint8_t>(1 + rng() % 120);
                rig.record([st, k](Pattern& p) { p.tracks[0].steps[st].notes[0].key = k; });
                proc_writes[st] = k;
                break;
            }
            case 1: {
                const auto st = static_cast<size_t>(rng() % 64);
                const auto v = static_cast<int16_t>(1 + rng() % 30000);
                rig.editor.edit([st, v](Pattern& p) { p.tracks[1].steps[st].mods[0].value = v; });
                edit_writes[st] = v;
                ++r.edits;
                break;
            }
            case 2: rig.editor.flush(rig.down); rig.down.pump(1); break;
            case 3: rig.block(); break;
            case 4: rig.proc.pump(rig.up); break;
            case 5: rig.up.pump(1); break;
        }
    }
    rig.settle(256);

    const auto s = rig.live();
    for (auto st = size_t{}; st < 64; ++st) {
        if (proc_writes[st] != 0 && s.tracks[0].steps[st].notes[0].key != proc_writes[st]) ++r.lost_proc;
        if (edit_writes[st] != 0 && s.tracks[1].steps[st].mods[0].value != edit_writes[st]) ++r.lost_edit;
    }
    r.refusals = rig.proc.rejections();
    r.converged = std::memcmp(&rig.editor.view(), &s, sizeof(Pattern)) == 0;
    return r;
}

auto test_fuzz() -> void
{
    constexpr auto seeds = 8u;
    std::printf("\nrandomized interleaving under CAS (20k ops x %u seeds)\n", seeds);

    auto total = Fuzz_result{};
    for (auto seed = 1u; seed <= seeds; ++seed) {
        const auto r = run_fuzz(seed);
        total.lost_proc += r.lost_proc;
        total.lost_edit += r.lost_edit;
        total.edits += r.edits;
        total.refusals += r.refusals;
        total.converged = total.converged && r.converged;
    }

    std::printf("  ....  %d lost proc, %d lost edit over %d edits, %zu refusals\n",
                total.lost_proc, total.lost_edit, total.edits, total.refusals);
    expect_eq(total.lost_proc, 0, "every processor value present");
    expect_eq(total.lost_edit, 0, "every editor value present");
    expect(total.converged, "editor view is byte-identical to processor state after draining");
}

// MARK: - cost

auto test_cost() -> void
{
    std::printf("\nthe extra walk CAS costs on the audio thread\n");

    auto dst = Pattern{};
    auto base = Pattern{};
    auto next = Pattern{};
    auto sink = size_t{};

    constexpr auto reps = 20000;
    const auto time = [&](auto&& f) {
        const auto t0 = std::chrono::steady_clock::now();
        for (auto i = 0; i < reps; ++i) {
            next.tracks[0].steps[i % 64].notes[0].key = static_cast<uint8_t>(1 + i % 120);
            sink += f();
            base = next;
        }
        return std::chrono::duration<double, std::nano>(
                   std::chrono::steady_clock::now() - t0).count() / reps;
    };

    const auto merge_ns = time([&] { return tiny::state::merge_into(dst, base, next); });
    base = Pattern{}; next = Pattern{}; dst = Pattern{};
    const auto drift_ns = time([&] { return tiny::state::drift_from(dst, base, next); });
    if (sink == 0xffffffff) std::printf(" ");

    std::printf("  ....  merge %.0f ns, drift check %.0f ns, CAS total %.0f ns"
                "  (%.2f%% of a 2.67 ms block)\n",
                merge_ns, drift_ns, merge_ns + drift_ns, (merge_ns + drift_ns) / 2.67e6 * 100.0);
    expect(merge_ns + drift_ns < 2.67e6 * 0.01, "still under 1% of the block budget");
}

} // namespace

auto main() -> int
{
    test_stale_read_modify_write();
    test_policy_per_edit();
    test_editor_replaces_the_document();
    test_coalescing_takes_the_stricter();
    test_refusal_is_not_a_loss();
    test_refusal_coalesced_with_a_later_apply();
    test_refusal_of_a_coalesced_pair();
    test_fresh_patch_over_a_staged_stale_one();
    test_drag_through_a_refusal();
    test_retry_rate();
    test_undo_through_refusal();
    test_fuzz();
    test_cost();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
