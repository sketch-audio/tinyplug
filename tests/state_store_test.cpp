#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <tiny_core/tiny_state.hpp>

#include "support/pipe.hpp"
#include "support/undo_log.hpp"
#include "support/sequencer.hpp"
#include <tiny_core/tiny_state.hpp>

namespace {

using demo::Pattern;
using tiny::state::Store;
using tiny::state::merge_into;

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

// MARK: - merge

struct Small {
    uint32_t a{}, b{}, c{}, d{};
};

auto test_merge() -> void
{
    std::printf("merge\n");

    {
        auto dst = Small{1, 2, 3, 4};
        auto base = Small{1, 2, 3, 4};
        auto next = Small{1, 9, 3, 4};
        const auto conflicts = merge_into(dst, base, next);
        expect(dst.b == 9, "the changed field lands");
        expect(dst.a == 1 && dst.c == 3 && dst.d == 4, "untouched fields are untouched");
        expect_eq(static_cast<long long>(conflicts), 0, "no conflict when dst matches base");
    }
    {
        // The whole point: someone else moved a different field since base was taken.
        auto dst = Small{1, 2, 3, 77};
        auto base = Small{1, 2, 3, 4};
        auto next = Small{1, 9, 3, 4};
        const auto conflicts = merge_into(dst, base, next);
        expect(dst.b == 9, "our edit lands");
        expect(dst.d == 77, "the other writer's field survives");
        expect_eq(static_cast<long long>(conflicts), 0, "disjoint edits are not conflicts");
    }
    {
        // Both writers touched the same field.
        auto dst = Small{1, 55, 3, 4};
        auto base = Small{1, 2, 3, 4};
        auto next = Small{1, 9, 3, 4};
        const auto conflicts = merge_into(dst, base, next);
        expect(dst.b == 9, "last writer wins");
        expect(conflicts == sizeof(uint32_t) || conflicts > 0, "overlap is reported as a conflict");
    }
    {
        auto dst = Small{5, 6, 7, 8};
        const auto before = dst;
        auto base = Small{1, 2, 3, 4};
        merge_into(dst, base, base);
        expect(std::memcmp(&dst, &before, sizeof(Small)) == 0, "base == next writes nothing");
    }
    {
        // Undo of a step is the same operation with the pair reversed.
        auto live = Small{1, 2, 3, 4};
        const auto from = live;
        auto to = live; to.c = 30;
        merge_into(live, from, to);
        live.d = 99;                      // something else happens afterwards
        merge_into(live, to, from);       // undo
        expect(live.c == 3, "undo reverts what the step changed");
        expect(live.d == 99, "undo leaves later writes alone");
    }
}

// MARK: - store

auto test_store() -> void
{
    std::printf("\nstore\n");

    auto store = Store<Small>{};
    auto out = Small{};

    {
        auto a = store.access();
        a.mutate().a = 7;
    }
    store.save(out);
    expect_eq(out.a, 7, "processor write is visible to save()");

    store.load(Small{7, 0, 0, 0}, Small{7, 3, 0, 0});
    store.save(out);
    expect_eq(out.b, 3, "save answers from the staged pair before it is consumed");

    { auto a = store.access(); (void) a; }
    store.save(out);
    expect_eq(out.b, 3, "staged edit landed on the next block");

    // Coalescing: two loads before a block runs must keep the ORIGINAL base, or the
    // first edit is silently dropped.
    auto v0 = Small{}; store.save(v0);
    auto v1 = v0; v1.c = 11;
    store.load(v0, v1);
    auto v2 = v1; v2.d = 22;
    store.load(v1, v2);
    { auto a = store.access(); (void) a; }
    store.save(out);
    expect(out.c == 11 && out.d == 22, "coalesced stage keeps both edits");

    // reset_to is a blind replace: the session document is the truth.
    store.reset_to(Small{100, 0, 0, 0});
    { auto a = store.access(); (void) a; }
    store.save(out);
    expect(out.a == 100 && out.c == 0 && out.d == 0, "session load replaces wholesale");
}

// MARK: - the headline: concurrent recorder vs editor

struct Soak_result {
    int lost_recordings{};
    int lost_edits{};
    size_t conflicts{};
    int edits{};
};

auto run_soak(tiny::state::Apply apply) -> Soak_result
{
    constexpr auto rounds = 2000;

    auto proc = tiny::state::Processor_side<Pattern>{};
    auto down = tiny::link::Pipe{/*immediate=*/true};
    auto up = tiny::link::Pipe{/*immediate=*/true};
    auto editor = tiny::state::Editor_side<Pattern>{};

    down.set_sink([&](std::span<const std::byte> b, uint32_t t) { proc.on_edit(b, t); });
    up.set_sink([&](std::span<const std::byte> b, uint32_t g) { editor.on_snapshot(b, g); });

    auto stop = std::atomic<bool>{false};

    // Phase 1: concurrent. "Audio thread" records into track 0, editor edits track 1.
    auto audio = std::thread([&] {
        for (auto i = 0; i < rounds; ++i) {
            auto block = proc.begin_block();
            const auto step = static_cast<size_t>(i % 64);
            block.mutate().tracks[0].steps[step].notes[0].key = static_cast<uint8_t>(1 + (i % 120));
            std::this_thread::yield();
        }
        stop.store(true);
    });

    auto edits = 0;
    while (!stop.load()) {
        const auto k = static_cast<int16_t>(1 + (edits % 63));
        editor.edit([=](Pattern& p) { p.tracks[1].steps[static_cast<size_t>(k)].mods[0].value = k; }, apply);
        editor.flush(down);

        // A shuttle tick spans several blocks, so the editor's view is a few blocks
        // stale by the time it builds its next patch. Snapshotting on every edit would
        // keep it artificially current and hide what the policy is actually doing.
        if (edits % 8 == 0) proc.pump(up);
        ++edits;
        std::this_thread::yield();
    }
    audio.join();
    for (auto i = 0; i < 4; ++i) { auto b = proc.begin_block(); (void) b; }

    auto out = Soak_result{};
    out.conflicts = proc.conflicts();
    out.edits = edits;

    // Phase 2, deterministic. Record a marker, then apply one edit built from the
    // editor's stale view — it has never received a snapshot, so its copy of track 0
    // is still empty. Merge must leave the marker alone; a whole-object overwrite must
    // erase it. The policy has to be passed explicitly: the default is Apply::Retry,
    // which would refuse rather than demonstrate anything.
    { auto b = proc.begin_block(); b.mutate().tracks[0].steps[7].notes[0].key = 123; }

    auto before = Pattern{};
    proc.snapshot(before);
    if (before.tracks[0].steps[7].notes[0].key != 123) ++out.lost_recordings; // sanity

    editor.edit([](Pattern& p) { p.tracks[1].steps[0].mods[1].value = 42; }, apply);
    editor.flush(down);
    { auto b = proc.begin_block(); (void) b; }

    auto final_state = Pattern{};
    proc.snapshot(final_state);

    if (final_state.tracks[0].steps[7].notes[0].key != 123) ++out.lost_recordings;
    if (final_state.tracks[1].steps[0].mods[1].value != 42) ++out.lost_edits;

    return out;
}

auto test_soak() -> void
{
    std::printf("\nconcurrent recorder + editor (%zu-byte state)\n", sizeof(Pattern));

    const auto merged = run_soak(tiny::state::Apply::Merge);
    std::printf("  ....  merge: %d lost recordings, %d lost edits, %zu conflicting bytes, %d edits\n",
                merged.lost_recordings, merged.lost_edits, merged.conflicts, merged.edits);
    expect(merged.lost_recordings == 0, "merge: no recorded note is clobbered by an editor edit");
    expect(merged.lost_edits == 0, "merge: no editor edit is clobbered by the recorder");
    expect(merged.conflicts == 0, "merge: disjoint writers report no conflicts");

    const auto blind = run_soak(tiny::state::Apply::Overwrite);
    std::printf("  ....  blind: %d lost recordings, %d lost edits, %d edits\n",
                blind.lost_recordings, blind.lost_edits, blind.edits);
    expect(blind.lost_recordings > 0, "blind replace DOES clobber the recorder (this is what merge buys)");
}

// MARK: - cost

template<size_t N>
auto bench_one(const char* label) -> void
{
    auto a = std::vector<std::byte>(N);
    auto b = std::vector<std::byte>(N);
    auto dst = std::vector<std::byte>(N);
    std::memset(a.data(), 0xAB, N);
    std::memcpy(b.data(), a.data(), N);
    b[N / 2] = std::byte{0x01}; // one changed byte, the common case

    constexpr auto iters = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (auto i = 0; i < iters; ++i) tiny::state::merge(dst.data(), a.data(), b.data(), N);
    const auto t1 = std::chrono::steady_clock::now();
    for (auto i = 0; i < iters; ++i) std::memcpy(dst.data(), b.data(), N);
    const auto t2 = std::chrono::steady_clock::now();

    const auto ns = [](auto d) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / double{iters};
    };
    std::printf("  %-8s merge %8.2f ns   memcpy %8.2f ns\n", label, ns(t1 - t0), ns(t2 - t1));
}

auto test_cost() -> void
{
    std::printf("\ncost per operation (one byte changed)\n");
    bench_one<4 * 1024>("4 KB");
    bench_one<26632>("26 KB");
    bench_one<64 * 1024>("64 KB");
    bench_one<256 * 1024>("256 KB");
    bench_one<1024 * 1024>("1 MB");
    std::printf("  ....  a 2.67 ms block at 128/48k is the budget to compare against\n");
}

} // namespace

auto main() -> int
{
    test_merge();
    test_store();
    test_soak();
    test_cost();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
