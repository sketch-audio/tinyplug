#include <cstdio>
#include <cstring>
#include <type_traits>

#include "support/sequencer.hpp"
#include <tiny_core/tiny_state.hpp>

#include "support/pipe.hpp"
#include "support/undo_log.hpp"

namespace {

using demo::Pattern;
using tiny::state::Writers;

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

// MARK: - what each mode removes from the API

template<typename B, typename = void>
struct has_mutate : std::false_type {};
template<typename B>
struct has_mutate<B, std::void_t<decltype(std::declval<B&>().mutate())>> : std::true_type {};

template<typename P, typename = void>
struct has_pump : std::false_type {};
template<typename P>
struct has_pump<P, std::void_t<decltype(std::declval<P&>().pump(std::declval<tiny::link::Pipe&>()))>>
    : std::true_type {};

template<typename E, typename = void>
struct has_edit : std::false_type {};
template<typename E>
struct has_edit<E, std::void_t<decltype(std::declval<E&>().edit([](Pattern&) {}))>> : std::true_type {};

template<typename E, typename = void>
struct has_on_snapshot : std::false_type {};
template<typename E>
struct has_on_snapshot<E, std::void_t<decltype(std::declval<E&>().on_snapshot(
    std::declval<std::span<const std::byte>>(), 0u))>> : std::true_type {};

template<Writers W> using Proc = tiny::state::Processor_side<Pattern, W>;
template<Writers W> using Edit = tiny::state::Editor_side<Pattern, W>;

auto test_api_surface() -> void
{
    std::printf("what the declaration removes\n");

    static_assert(!has_mutate<typename Proc<Writers::Editor>::Block>::value);
    static_assert( has_mutate<typename Proc<Writers::Processor>::Block>::value);
    static_assert( has_mutate<typename Proc<Writers::Both>::Block>::value);

    static_assert(!has_pump<Proc<Writers::Editor>>::value);
    static_assert( has_pump<Proc<Writers::Processor>>::value);

    static_assert( has_edit<Edit<Writers::Editor>>::value);
    static_assert(!has_edit<Edit<Writers::Processor>>::value);
    static_assert( has_edit<Edit<Writers::Both>>::value);

    static_assert(!has_on_snapshot<Edit<Writers::Editor>>::value);
    static_assert( has_on_snapshot<Edit<Writers::Processor>>::value);

    expect(true, "Writers::Editor    has no Block::mutate and no pump");
    expect(true, "Writers::Processor has no Editor_side::edit");
    expect(true, "the contract is a compile error, not a comment");
}

// MARK: - Writers::Editor

auto test_editor_only() -> void
{
    std::printf("\nWriters::Editor -- the UI authors, nothing contests\n");

    auto proc = Proc<Writers::Editor>{};
    auto editor = Edit<Writers::Editor>{};
    auto down = tiny::link::Pipe{false};
    auto undo = tiny::test::Undo_log{};
    down.set_sink([&](std::span<const std::byte> b, uint32_t t) { proc.on_edit(b, t); });

    const auto tick = [&] {
        editor.flush(down);
        down.pump();
        auto b = proc.begin_block();
        (void)b;
    };

    editor.edit([](Pattern& p) { p.tracks[2].steps[9].notes[0].key = 60; });

    // No round trip, no gate, no sequence number: the view IS the authority, so the
    // step can be recorded the moment the gesture ends.
    expect(!editor.in_flight(), "nothing is ever outstanding");
    expect(editor.commit(undo), "commit succeeds immediately, with no snapshot in sight");
    expect_eq(static_cast<long long>(undo.steps()), 1, "one step, recorded at gesture time");

    tick();
    auto live = Pattern{};
    proc.snapshot(live);
    expect_eq(live.tracks[2].steps[9].notes[0].key, 60, "the processor has it after one block");

    expect_eq(static_cast<long long>(down.sent_bytes()), static_cast<long long>(sizeof(Pattern)),
              "the down message is one copy of T, not a pair");

    // Undo, in a mode with no return channel at all.
    const auto on_param = [](uint32_t, double) {};
    const auto on_state = [&](const std::byte* b, const std::byte* n, size_t sz) {
        editor.apply_replay(b, n, sz);
    };
    undo.undo(on_param, on_state);
    tick();
    proc.snapshot(live);
    expect_eq(live.tracks[2].steps[9].notes[0].key, 0, "undo reverts, with no acknowledgement");

    undo.redo(on_param, on_state);
    tick();
    proc.snapshot(live);
    expect_eq(live.tracks[2].steps[9].notes[0].key, 60, "and redo restores");
}

// MARK: - Writers::Processor

auto test_processor_only() -> void
{
    std::printf("\nWriters::Processor -- the audio thread authors, the UI only reverts\n");

    auto proc = Proc<Writers::Processor>{};
    auto editor = Edit<Writers::Processor>{};
    auto down = tiny::link::Pipe{false};
    auto up = tiny::link::Pipe{false};
    auto undo = tiny::test::Undo_log{};
    down.set_sink([&](std::span<const std::byte> b, uint32_t t) { proc.on_edit(b, t); });
    up.set_sink([&](std::span<const std::byte> b, uint32_t t) { editor.on_snapshot(b, t); });

    const auto tick = [&] {
        editor.flush(down);
        down.pump();
        { auto b = proc.begin_block(); (void)b; }
        proc.pump(up);
        up.pump();
    };
    const auto settle = [&] { for (auto i = 0; i < 6; ++i) tick(); };
    const auto live = [&] { auto p = Pattern{}; auto s = uint32_t{}; proc.snapshot(p); (void)s; return p; };

    // A recording pass: the audio thread authors four notes.
    for (auto i = 0; i < 4; ++i) {
        auto b = proc.begin_block();
        b.mutate().tracks[0].steps[i].notes[0] = {uint8_t(60 + i), 100};
    }
    settle();

    expect_eq(live().tracks[0].steps[3].notes[0].key, 63, "the take reached the document");
    expect_eq(editor.view().tracks[0].steps[3].notes[0].key, 63, "and the editor sees it");

    // The take is a gesture, and it is undoable -- which is the whole reason this mode
    // still needs the merge and both directions of the wire.
    expect(editor.commit(undo), "the take closes out as an undo step");
    expect_eq(static_cast<long long>(undo.steps()), 1, "one step");

    const auto on_param = [](uint32_t, double) {};
    const auto on_state = [&](const std::byte* b, const std::byte* n, size_t sz) {
        editor.apply_replay(b, n, sz);   // edit() is gone; reverting is not authoring
    };
    undo.undo(on_param, on_state);
    settle();

    expect_eq(live().tracks[0].steps[3].notes[0].key, 0, "undo pushes the revert back down");
    expect_eq(live().tracks[0].steps[0].notes[0].key, 0, "the whole take");

    undo.redo(on_param, on_state);
    settle();
    expect_eq(live().tracks[0].steps[3].notes[0].key, 63, "redo replays it");

    // And the processor keeps authoring afterwards, through the same merge.
    { auto b = proc.begin_block(); b.mutate().tracks[1].steps[0].notes[0] = {40, 100}; }
    settle();
    expect_eq(editor.view().tracks[1].steps[0].notes[0].key, 40, "recording continues past the undo");
}

// MARK: - cost

auto test_cost() -> void
{
    std::printf("\nwhat the declaration is worth, at sizeof(T) = %zu\n", sizeof(Pattern));

    const auto ed = sizeof(tiny::state::Store<Pattern, Writers::Editor>)
                  + sizeof(Edit<Writers::Editor>);
    const auto bo = sizeof(tiny::state::Store<Pattern, Writers::Both>)
                  + sizeof(Edit<Writers::Both>);

    std::printf("  ....  store + editor:  Editor %6zu B   Both %6zu B   (%.0f%% of Both)\n",
                ed, bo, 100.0 * double(ed) / double(bo));
    std::printf("  ....  down per gesture: Editor %5zu B   Both %5zu B\n",
                sizeof(Pattern), 2 * sizeof(Pattern));
    std::printf("  ....  up per tick:      Editor  none      Both %5zu B\n",
                sizeof(tiny::state::Ack) + sizeof(Pattern));

    expect(ed < bo, "Writers::Editor is materially smaller");
}

} // namespace

auto main() -> int
{
    test_api_surface();
    test_editor_only();
    test_processor_only();
    test_cost();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
