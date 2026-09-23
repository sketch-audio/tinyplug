#include <cstdio>
#include <cstring>
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

// A whole plug-in. `coupled` picks CLAP/AUv2/AUv3 (shared memory, delivery on call)
// versus VST3/AAX (copied, delivered when the host pumps).
struct Rig {

    explicit Rig(bool coupled, size_t undo_budget = 4u * 1024 * 1024)
        : down{coupled}, up{coupled}, undo{undo_budget}
    {
        down.set_sink([this](std::span<const std::byte> b, uint32_t t) { proc.on_edit(b, t); });
        up.set_sink([this](std::span<const std::byte> b, uint32_t g) { editor.on_snapshot(b, g); });
    }

    tiny::state::Processor_side<Pattern> proc{};
    tiny::state::Editor_side<Pattern> editor{};
    tiny::link::Pipe down;   // editor -> processor, {base, next}
    tiny::link::Pipe up;     // processor -> editor, snapshot
    Undo_log undo;

    // One audio block.
    auto block() -> void { auto b = proc.begin_block(); (void) b; }

    auto record(size_t track, size_t step, uint8_t key) -> void
    {
        auto b = proc.begin_block();
        b.mutate().tracks[track].steps[step].notes[0].key = key;
    }

    // The host delivering messages, plus the ~16 ms shuttle tick.
    auto host_tick() -> void
    {
        editor.flush(down);
        down.pump();
        block();
        proc.pump(up);
        up.pump();
    }

    auto live(size_t track, size_t step) -> uint8_t
    {
        auto s = Pattern{};
        proc.snapshot(s);
        return s.tracks[track].steps[step].notes[0].key;
    }

    auto replay(bool is_undo) -> bool
    {
        const auto on_param = [&](uint32_t addr, double v) { params[addr] = v; };
        const auto on_state = [&](const std::byte* base, const std::byte* next, size_t n) {
            editor.apply_replay(base, next, n);
        };
        return is_undo ? undo.undo(on_param, on_state) : undo.redo(on_param, on_state);
    }

    std::vector<double> params = std::vector<double>(8, 0.0);
};

// MARK: - coupled round trip

auto test_coupled() -> void
{
    std::printf("coupled (CLAP / AUv2 / AUv3)\n");

    auto rig = Rig{true};

    rig.editor.edit([](Pattern& p) { p.tracks[2].steps[9].notes[0].key = 60; });
    expect(rig.editor.view().tracks[2].steps[9].notes[0].key == 60, "optimistic view shows the edit immediately");
    expect(rig.editor.in_flight(), "and reports it as outstanding");

    rig.host_tick();
    expect_eq(rig.live(2, 9), 60, "processor has the edit after one block");
    expect(!rig.editor.in_flight(), "editor reconciled once the snapshot came back");

    rig.editor.commit(rig.undo);
    expect_eq(static_cast<long long>(rig.undo.steps()), 1, "one undo step recorded");

    rig.replay(true);
    rig.host_tick();
    expect_eq(rig.live(2, 9), 0, "undo reverted it");

    rig.replay(false);
    rig.host_tick();
    expect_eq(rig.live(2, 9), 60, "redo restored it");
}

// MARK: - record while the editor is closed

auto test_record_closed_then_undo() -> void
{
    std::printf("\nrecord with the editor closed, then open and undo (VST3-style link)\n");

    auto rig = Rig{false};

    // Editor closed: nothing pumps, nothing is sent up. The processor just records.
    for (auto i = 0; i < 16; ++i) rig.record(0, static_cast<size_t>(i), static_cast<uint8_t>(40 + i));
    expect_eq(rig.up.sent_msgs(), 0, "no traffic at all while the window is closed");
    expect_eq(static_cast<long long>(rig.undo.steps()), 0, "and no undo step yet");

    // Window opens: one snapshot arrives, one commit, one coalesced step for the take.
    rig.proc.pump(rig.up);
    rig.up.pump();
    expect_eq(rig.editor.view().tracks[0].steps[3].notes[0].key, 43, "editor sees what was recorded");

    rig.editor.commit(rig.undo);
    expect_eq(static_cast<long long>(rig.undo.steps()), 1, "the whole take is ONE undo step");

    // A note recorded after the commit must survive the undo.
    rig.record(0, 40, 99);
    rig.host_tick();

    rig.replay(true);
    rig.host_tick();

    expect_eq(rig.live(0, 3), 0, "undo removed the recorded take");
    expect_eq(rig.live(0, 40), 99, "the note recorded after the commit survived the undo");

    rig.replay(false);
    rig.host_tick();
    expect_eq(rig.live(0, 3), 43, "redo restored the take");
    expect_eq(rig.live(0, 40), 99, "and still did not disturb the later note");
}

// MARK: - a gesture mixing params and state

auto test_mixed_gesture() -> void
{
    std::printf("\none gesture mixing a param and a state edit\n");

    auto rig = Rig{true};
    rig.params[1] = 0.25;

    rig.undo.begin_gesture();
    rig.undo.record_param(1, 0.25, 0.75);
    rig.params[1] = 0.75;
    rig.editor.edit([](Pattern& p) { p.tracks[1].steps[0].mods[2].value = 7; });
    rig.host_tick();
    rig.editor.commit(rig.undo);          // lands inside the open gesture
    rig.undo.end_gesture();

    expect_eq(static_cast<long long>(rig.undo.steps()), 1, "param + state coalesce into one step");

    rig.replay(true);
    rig.host_tick();
    expect(rig.params[1] == 0.25, "param reverted");
    expect_eq(rig.live(1, 0) == 0 ? 0 : 1, 0, "note untouched");

    auto s = Pattern{};
    rig.proc.snapshot(s);
    expect_eq(s.tracks[1].steps[0].mods[2].value, 0, "state reverted in the same step");
}

// MARK: - budget

auto test_budget() -> void
{
    std::printf("\nbyte-budgeted history (%zu-byte state)\n", sizeof(Pattern));

    const auto budget = size_t{1024 * 1024};
    auto rig = Rig{true, budget};

    for (auto i = 0; i < 40; ++i) {
        rig.editor.edit([=](Pattern& p) {
            p.tracks[0].steps[static_cast<size_t>(i)].notes[0].key = static_cast<uint8_t>(1 + i);
        });
        rig.host_tick();
        rig.editor.commit(rig.undo);
    }

    const auto per_step = 2 * sizeof(Pattern);
    std::printf("  ....  %zu steps held, %zu evicted, %zu bytes (%zu per step)\n",
                rig.undo.steps(), rig.undo.evicted(), rig.undo.bytes(), per_step);

    expect(rig.undo.bytes() <= budget, "history stays inside the budget");
    expect(rig.undo.evicted() > 0, "oldest steps are evicted rather than the cap being a hard failure");
    expect_eq(static_cast<long long>(rig.undo.steps()), static_cast<long long>(budget / per_step),
              "held steps == budget / (2 * sizeof(T))");
}

// MARK: - what the wire costs

auto test_traffic() -> void
{
    std::printf("\nwire traffic over one simulated second (VST3-style link)\n");

    {
        // Recording at 120 BPM sixteenths: 8 state changes/sec, shuttle ticks at 16 ms.
        auto rig = Rig{false};
        rig.up.reset_counters();
        rig.down.reset_counters();

        for (auto tick = 0; tick < 62; ++tick) {       // ~62 x 16 ms
            if (tick % 8 == 0) rig.record(0, static_cast<size_t>(tick % 64), 64);
            rig.host_tick();
        }
        std::printf("  recording   up %6zu KB/s in %2zu msgs   down %4zu KB/s in %2zu msgs\n",
                    rig.up.sent_bytes() / 1024, rig.up.sent_msgs(),
                    rig.down.sent_bytes() / 1024, rig.down.sent_msgs());
        expect(rig.up.sent_msgs() <= 10, "one snapshot per actual change, not per tick");
    }
    {
        // Editing: two gestures a second.
        auto rig = Rig{false};
        rig.up.reset_counters();
        rig.down.reset_counters();

        for (auto tick = 0; tick < 62; ++tick) {
            if (tick % 31 == 0) {
                rig.editor.edit([=](Pattern& p) {
                    p.tracks[1].steps[static_cast<size_t>(tick % 64)].notes[0].key = 70;
                });
            }
            rig.host_tick();
        }
        std::printf("  editing     up %6zu KB/s in %2zu msgs   down %4zu KB/s in %2zu msgs\n",
                    rig.up.sent_bytes() / 1024, rig.up.sent_msgs(),
                    rig.down.sent_bytes() / 1024, rig.down.sent_msgs());
    }
    {
        // Idle: transport running, nothing changing.
        auto rig = Rig{false};
        rig.up.reset_counters();
        rig.down.reset_counters();
        for (auto tick = 0; tick < 62; ++tick) rig.host_tick();
        std::printf("  idle        up %6zu KB/s in %2zu msgs   down %4zu KB/s in %2zu msgs\n",
                    rig.up.sent_bytes() / 1024, rig.up.sent_msgs(),
                    rig.down.sent_bytes() / 1024, rig.down.sent_msgs());
        expect_eq(static_cast<long long>(rig.up.sent_msgs()), 0, "idle costs nothing");
    }
}

} // namespace

auto main() -> int
{
    test_coupled();
    test_record_closed_then_undo();
    test_mixed_gesture();
    test_budget();
    test_traffic();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
