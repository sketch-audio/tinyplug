// tinyplug's editor half of the state document (`state::Editor_link`) wired to the real
// `Undo_history`, over the two wirings the wrappers use: in-process and remote.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <tinyplug/tiny_state_link.hpp>
#include <tinyplug/undo_history.hpp>

#include "support/pipe.hpp"

namespace {

namespace state = tiny::state;

template<state::Writers W>
struct Doc {
    static constexpr auto writers = W;
    std::array<std::uint8_t, 16> level{};
    std::array<std::uint8_t, 16> recorded{}; // Written by the processor in Both mode.
};

using Editor_doc = Doc<state::Writers::Editor>;
using Both_doc = Doc<state::Writers::Both>;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

// One editor frame's worth of framework work, in run_frame's order.
template<typename Link>
auto frame(Link& link, tiny::Undo_history& history, tiny::Action_queue& actions) -> void
{
    link.sync();
    history.perform_actions(actions.actor());
    link.sync();
}

// One host block, returning what the processor saw.
template<typename M>
auto block(state::Processor_for<M>& processor) -> M
{
    auto b = processor.begin_block();
    return b.get();
}

// MARK: - in process, editor writes

auto test_in_process_undo_redo() -> void
{
    std::printf("in process, Writers::Editor\n");

    auto processor = state::Processor_for<Editor_doc>{};
    auto link = state::Editor_link<Editor_doc>{};
    auto history = tiny::Undo_history{};
    auto actions = tiny::Action_queue{};
    state::connect_in_process(link, processor);
    link.bind(history);
    auto editor = link.actor();

    editor.edit([](Editor_doc& d) { d.level[3] = 80; });
    editor.edit([](Editor_doc& d) { d.level[4] = 90; });
    editor.commit();
    frame(link, history, actions);
    expect(block(processor).level[3] == 80 && block(processor).level[4] == 90, "a stroke reaches the processor");
    expect(history.can_undo(), "a commit makes one undo step");

    history.actor().undo();
    frame(link, history, actions);
    const auto undone = block(processor);
    expect(editor.view().level[3] == 0 && undone.level[3] == 0 && undone.level[4] == 0, "undo reverts both the view and the processor");
    expect(history.can_redo(), "and can be redone");

    history.actor().redo();
    frame(link, history, actions);
    expect(block(processor).level[4] == 90 && editor.view().level[4] == 90, "redo re-applies it");

    // An edit left uncommitted still becomes a step before an undo walks past it.
    editor.edit([](Editor_doc& d) { d.level[0] = 10; });
    frame(link, history, actions);
    history.actor().undo();
    frame(link, history, actions);
    expect(editor.view().level[0] == 0 && editor.view().level[4] == 90, "undo commits a pending edit first, then reverts only it");
}

// MARK: - remote, both write

// VST3 / AAX: edits go down a queued pipe, snapshots come back through an inbox.
struct Remote {
    state::Processor_for<Both_doc> processor{};
    state::Editor_link<Both_doc> link{};
    state::Snapshot_inbox<Both_doc> inbox{};
    tiny::link::Pipe down{false};
    tiny::Undo_history history{};
    tiny::Action_queue actions{};

    Remote()
    {
        down.set_sink([this](std::span<const std::byte> bytes, std::uint32_t seq) { processor.on_edit(bytes, seq); });
        state::connect_remote(link, [this](std::span<const std::byte> bytes, std::uint32_t seq) {
            return down.send(bytes, seq);
        }, inbox);
        link.bind(history);
    }

    // The host delivering what was sent, a block running, and a snapshot coming back.
    auto round_trip() -> void
    {
        down.pump();
        block(processor);
        processor.pump([this](std::span<const std::byte> bytes, std::uint32_t gen) {
            return state::post_snapshot<Both_doc>(inbox, bytes, gen);
        });
    }
};

auto test_remote_both_write() -> void
{
    std::printf("remote, Writers::Both\n");
    auto r = Remote{};
    auto editor = r.link.actor();

    editor.edit([](Both_doc& d) { d.level[2] = 50; });
    editor.commit();
    frame(r.link, r.history, r.actions);
    expect(editor.in_flight() && !r.history.can_undo(), "a step waits while its edit is unconfirmed");

    r.round_trip();
    frame(r.link, r.history, r.actions);
    expect(!editor.in_flight() && r.history.can_undo(), "the step closes once the processor confirms");

    // The processor records into its own region after the step closed.
    {
        auto b = r.processor.begin_block();
        b.mutate().recorded[7] = 64;
    }
    r.processor.pump([&](std::span<const std::byte> bytes, std::uint32_t gen) {
        return state::post_snapshot<Both_doc>(r.inbox, bytes, gen);
    });
    frame(r.link, r.history, r.actions);
    expect(editor.view().recorded[7] == 64, "the processor's write reaches the view");

    // Anything the processor wrote since the last step becomes a step of its own at the
    // next commit point, so the first undo takes back the recording, the second the edit.
    r.history.actor().undo();
    frame(r.link, r.history, r.actions);
    r.round_trip();
    frame(r.link, r.history, r.actions);
    auto after = block(r.processor);
    expect(after.recorded[7] == 0 && after.level[2] == 50, "the first undo reverts the processor's recording");

    // A write that lands while an undo is still on the wire survives it.
    r.history.actor().undo();
    frame(r.link, r.history, r.actions);
    r.down.pump();
    {
        auto b = r.processor.begin_block();
        b.mutate().recorded[8] = 33;
    }
    block(r.processor);
    r.processor.pump([&](std::span<const std::byte> bytes, std::uint32_t gen) {
        return state::post_snapshot<Both_doc>(r.inbox, bytes, gen);
    });
    frame(r.link, r.history, r.actions);
    after = block(r.processor);
    expect(after.level[2] == 0, "the second reverts the editor's edit");
    expect(after.recorded[8] == 33 && r.link.view().recorded[8] == 33, "and a write made meanwhile survives it");
}

// MARK: - reset seed

// AAX rebuilds the processor at a reset and seeds it from the data model.
auto test_reseeded_processor() -> void
{
    std::printf("reseeded processor (AAX reset)\n");
    auto r = Remote{};
    auto editor = r.link.actor();

    for (auto i = 0; i < 3; ++i) {
        editor.edit([i](Both_doc& d) { d.level[static_cast<size_t>(i)] = 20; });
        frame(r.link, r.history, r.actions);
        r.round_trip();
        frame(r.link, r.history, r.actions);
    }
    editor.edit([](Both_doc& d) { d.level[9] = 99; });
    frame(r.link, r.history, r.actions); // Sent, never delivered: the reset eats it.

    const auto& side = r.link.side();
    const auto seed_value = side.sent();
    const auto seed_applied = side.sent_at();
    const auto seed_gen = side.generation();

    r.processor.~Processor_side();
    new (&r.processor) state::Processor_for<Both_doc>{};
    r.processor.on_session_load(seed_value, seed_applied);
    r.processor.seed_generation(seed_gen);
    r.down.pump(0); // Nothing delivered.
    while (r.down.pending() > 0) r.down.pump(); // The stale patch still arrives; it must be harmless.

    block(r.processor);
    r.processor.pump([&](std::span<const std::byte> bytes, std::uint32_t gen) {
        return state::post_snapshot<Both_doc>(r.inbox, bytes, gen);
    });
    frame(r.link, r.history, r.actions);

    expect(block(r.processor).level[9] == 99, "the reseeded processor holds the edit the reset swallowed");
    expect(!editor.in_flight(), "and the editor's outstanding edits retire against it");
    expect(r.link.side().generation() > seed_gen, "snapshots after the reset are not dropped as late");
}

// MARK: - host loads

// What each wrapper's restore path does: params into one host-load step, then the document into the same one.
template<typename M>
auto host_load(state::Processor_for<M>* processor, state::Editor_link<M>& link, tiny::Undo_history& history,
               const M& doc, double param_from, double param_to) -> void
{
    auto changes = std::vector<tiny::Set_param>{};
    history.push_host_load(std::array{param_from}, std::array{param_to}, changes);
    if (processor) processor->on_session_load(doc);
    link.load(doc);
}

auto test_host_load_undo() -> void
{
    std::printf("host load, in process\n");

    auto processor = state::Processor_for<Editor_doc>{};
    auto link = state::Editor_link<Editor_doc>{};
    auto history = tiny::Undo_history{};
    auto actions = tiny::Action_queue{};
    state::connect_in_process(link, processor);
    link.bind(history);
    auto editor = link.actor();

    editor.edit([](Editor_doc& d) { d.level[0] = 5; });
    editor.commit();
    frame(link, history, actions);

    auto doc = Editor_doc{};
    doc.level[1] = 77;
    host_load(&processor, link, history, doc, 0.0, 0.5);
    frame(link, history, actions);
    const auto loaded = block(processor);
    expect(loaded.level[1] == 77 && loaded.level[0] == 0 && editor.view().level[1] == 77, "the load replaces both copies");

    history.actor().undo();
    frame(link, history, actions);
    const auto undone = block(processor);
    expect(editor.view().level[0] == 5 && editor.view().level[1] == 0, "one undo reverts the load's document");
    expect(undone.level[0] == 5 && undone.level[1] == 0, "on the processor too");
    expect(history.can_undo(), "the edit before it is still its own step");

    history.actor().redo();
    frame(link, history, actions);
    expect(block(processor).level[1] == 77, "redo re-applies the load");

    // A document-only load still makes a step; an identical load makes none.
    auto steps = [&] {
        auto h = std::size_t{};
        for (; history.can_undo(); ++h) {
            history.actor().undo();
            frame(link, history, actions);
        }
        for (auto i = std::size_t{}; i < h; ++i) {
            history.actor().redo();
            frame(link, history, actions);
        }
        return h;
    };
    const auto before = steps();
    auto doc2 = doc;
    doc2.level[2] = 9;
    host_load(&processor, link, history, doc2, 0.5, 0.5);
    expect(steps() == before + 1, "a load that moves only the document is one step");
    host_load(&processor, link, history, doc2, 0.5, 0.5);
    expect(steps() == before + 1, "a load that moves nothing is none");
}

// AAX: the data model loads, and the algorithm hears it only through the next patch.
auto test_load_resend() -> void
{
    std::printf("host load, resent (AAX)\n");
    auto r = Remote{};
    auto editor = r.link.actor();

    editor.edit([](Both_doc& d) { d.level[5] = 55; });
    frame(r.link, r.history, r.actions); // Sent, still on the wire when the load lands.

    auto doc = Both_doc{};
    doc.level[2] = 22;
    auto changes = std::vector<tiny::Set_param>{};
    r.history.push_host_load(std::array{0.0}, std::array{0.0}, changes);
    r.link.load(doc, state::Editor_link<Both_doc>::Resend::Yes);
    expect(r.link.flush(), "the load is sent even though the view matches what was sent");

    r.round_trip();
    frame(r.link, r.history, r.actions);
    const auto got = block(r.processor);
    expect(got.level[2] == 22 && got.level[5] == 0, "the Overwrite replaces the document, stale edit and all");
    expect(!editor.in_flight(), "and nothing is left waiting");
    expect(!r.link.flush(), "a second flush has nothing to send");
}

// An Overwrite replaces whatever is staged, even a replace whose bytes its base doesn't describe.
auto test_overwrite_over_staged_load() -> void
{
    std::printf("overwrite over a staged load\n");
    auto store = state::Store<Both_doc, state::Writers::Both>{};

    auto loaded = Both_doc{};
    loaded.level[0] = 1;
    loaded.level[1] = 2;
    store.reset_to(loaded); // A session load, not yet taken by a block.

    auto base = Both_doc{};  // The editor's idea of the document: stale.
    auto next = Both_doc{};
    next.level[4] = 4;
    store.load(base, next, 1, state::Apply::Overwrite);

    { auto a = store.access(); }
    auto got = Both_doc{};
    auto applied = std::uint32_t{};
    auto rejected = std::uint32_t{};
    store.read(got, applied, rejected);
    expect(got.level[0] == 0 && got.level[1] == 0 && got.level[4] == 4, "the document is exactly the Overwrite");
}

// VST3: the relay can pump after the controller seeded, from a block published before the load.
auto test_stale_snapshot() -> void
{
    std::printf("stale snapshot across a load\n");
    auto r = Remote{};

    {
        auto b = r.processor.begin_block();
        b.mutate().recorded[0] = 1; // Published, not yet sent.
    }

    auto doc = Both_doc{};
    doc.level[3] = 33;
    r.processor.on_session_load(doc);
    r.link.load(doc);
    r.processor.pump([&](std::span<const std::byte> bytes, std::uint32_t gen) {
        return state::post_snapshot<Both_doc>(r.inbox, bytes, gen);
    });
    frame(r.link, r.history, r.actions);
    expect(r.link.view().level[3] == 33 && r.link.view().recorded[0] == 0, "the old document does not come back");

    r.round_trip();
    frame(r.link, r.history, r.actions);
    expect(r.link.view().level[3] == 33 && block(r.processor).level[3] == 33, "the next snapshot confirms the load");
}

// An editor-side preset browser loads a record as an ordinary undoable edit.
auto test_load_record() -> void
{
    std::printf("editor load_record\n");

    auto processor = state::Processor_for<Editor_doc>{};
    auto link = state::Editor_link<Editor_doc>{};
    auto history = tiny::Undo_history{};
    auto actions = tiny::Action_queue{};
    state::connect_in_process(link, processor);
    link.bind(history);
    auto editor = link.actor();

    auto doc = Editor_doc{};
    doc.level[6] = 66;
    const auto record = state::encode_record(doc);
    expect(editor.load_record(record), "a record loads");
    editor.commit();
    frame(link, history, actions);
    expect(block(processor).level[6] == 66, "and reaches the processor");

    expect(!editor.load_record(std::span{record}.first(10)), "a truncated one is refused");
    history.actor().undo();
    frame(link, history, actions);
    expect(editor.view().level[6] == 0, "the load undoes like an edit");
}

// MARK: - budget

auto test_budget() -> void
{
    std::printf("byte budget\n");
    auto history = tiny::Undo_history{};
    history.set_state_budget(4 * 2 * sizeof(Editor_doc));

    auto a = Editor_doc{};
    for (auto i = 0; i < 10; ++i) {
        auto b = a;
        b.level[0] = static_cast<std::uint8_t>(i + 1);
        history.record_state(state::as_bytes(a), state::as_bytes(b), sizeof(Editor_doc));
        a = b;
    }

    auto steps = 0;
    auto actions = tiny::Action_queue{};
    auto replayed = std::uint8_t{};
    history.bind_state({.commit = [] { return false; },
                        .replay = [&](const std::byte*, const std::byte* next, std::size_t) {
                            replayed = static_cast<std::uint8_t>(next[0]);
                        }});
    while (history.can_undo()) {
        history.actor().undo();
        history.perform_actions(actions.actor());
        ++steps;
    }
    expect(steps == 4, "history keeps as many steps as the budget holds");
    expect(replayed == 6, "and evicts the oldest");
}

} // namespace

auto main() -> int
{
    test_in_process_undo_redo();
    std::printf("\n"); test_remote_both_write();
    std::printf("\n"); test_reseeded_processor();
    std::printf("\n"); test_host_load_undo();
    std::printf("\n"); test_load_resend();
    std::printf("\n"); test_overwrite_over_staged_load();
    std::printf("\n"); test_stale_snapshot();
    std::printf("\n"); test_load_record();
    std::printf("\n"); test_budget();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
