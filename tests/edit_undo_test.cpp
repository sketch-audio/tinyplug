// What the editor's actions become: `Edit_stream` gives every set a gesture, and `Undo_history` groups
// a frame's edits into steps (gestures, transactions, the frame amend window, state sources, held
// steps), through `view_impl::resolve_frame` as the wrappers run it.

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tinyplug/tiny_view.hpp>

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using namespace tiny;

auto start(uint32_t a) -> User_action { return Action_start{a}; }
auto end(uint32_t a) -> User_action { return Action_end{a}; }
auto set(uint32_t a, double v) -> User_action { return Set_param{a, v}; }
auto begin_tx() -> User_action { return Action_start{}; }
auto end_tx() -> User_action { return Action_end{}; }

auto describe(const User_action& action) -> std::string
{
    return std::visit(Inline_visitor{
        [](const Action_start& s) { return s.address ? std::format("S{}", *s.address) : std::string{"S"}; },
        [](const Action_end& e) { return e.address ? std::format("E{}", *e.address) : std::string{"E"}; },
        [](const Set_param& p) { return std::format("={}", p.address); },
        [](const Request_resize&) { return std::string{"R"}; },
    }, action);
}

auto describe(const std::vector<User_action>& actions) -> std::string
{
    auto out = std::string{};
    for (const auto& a : actions) out += (out.empty() ? "" : " ") + describe(a);
    return out;
}

auto normalized(const std::vector<User_action>& in) -> std::string
{
    auto stream = Edit_stream{};
    auto out = std::vector<User_action>{};
    stream.normalize(in, out);
    return describe(out);
}

// A non-parameter source, standing in for a document or the view: one byte, committed on request.
struct Fake_source {
    Undo_history* history{};
    Undo_history::Source source{Undo_history::Source::View};
    std::byte value{};
    std::byte baseline{};
    bool waiting{}; // In flight: a commit waits.

    auto bind(Undo_history& h) -> void
    {
        history = &h;
        h.bind_state(source, Undo_history::State_hooks{
            .commit = [this] { return commit(); },
            .replay = [this](const std::byte*, const std::byte* next, std::size_t) { value = baseline = *next; },
        });
    }

    auto commit() -> Undo_history::Commit
    {
        if (waiting) return Undo_history::Commit::Waiting;
        if (value == baseline) return Undo_history::Commit::None;
        history->record_state(source, &baseline, &value, 1);
        baseline = value;
        return Undo_history::Commit::Recorded;
    }

    // The processor confirmed: what was waiting records now.
    auto confirm() -> void
    {
        waiting = false;
        commit();
    }
};

// The wrapper's side of the frames: actions as a host would get them, and the editor's params.
struct Rig {
    Action_queue actions{};
    Undo_history history{};
    std::array<double, 4> params{};
    std::vector<User_action> host{}; // What the wrapper's action handler received.
    Ui_receiver receiver{};

    Rig()
    {
        history.bind_actions(actions.actor());
        receiver.action_handler = [this](const User_action& a) { host.push_back(a); };
    }

    // One frame: the editor's draw, then run_frame's tail.
    auto frame(const std::function<void(Action_queue::Actor)>& draw = {}) -> void
    {
        actions.clear();
        if (draw) draw(actions.actor());
        auto state = Processor_state{};
        state.params = params;
        view_impl::resolve_frame(receiver, params, actions, history, state, [](auto, auto) {});
    }

    auto push(std::initializer_list<User_action> list) -> void
    {
        frame([&](Action_queue::Actor a) { for (const auto& x : list) a.push(x); });
    }

    auto undo() -> void { history.actor().undo(); frame(); }
    auto redo() -> void { history.actor().redo(); frame(); }

    auto steps() -> int
    {
        auto n = 0;
        while (history.can_undo()) { undo(); ++n; }
        for (auto k = 0; k < n; ++k) redo();
        return n;
    }
};

auto add_stream_tests() -> void
{
    Tests::add("stream: a bare set becomes its own gesture", [] {
        expect_true(normalized({set(1, .5)}) == "S1 =1 E1", normalized({set(1, .5)}));
    });

    Tests::add("stream: a transaction touches each parameter before its first set and releases them at its end", [] {
        const auto got = normalized({begin_tx(), set(0, .1), set(1, .2), set(0, .3), end_tx()});
        expect_true(got == "S S0 =0 S1 =1 =0 E0 E1 E", got);
    });

    Tests::add("stream: a set inside another parameter's gesture joins it", [] {
        const auto got = normalized({start(0), set(1, .2), end(0)});
        expect_true(got == "S0 S1 =1 E1 E0", got);
    });

    Tests::add("stream: overlapping gestures release each parameter at its own end", [] {
        const auto got = normalized({start(0), start(1), set(0, .1), set(1, .2), end(0), set(1, .3), end(1)});
        expect_true(got == "S0 S1 =0 =1 E0 =1 E1", got);
    });

    Tests::add("stream: a stray end, or an end for a parameter only touched implicitly, is dropped", [] {
        expect_true(normalized({end(2)}).empty(), "stray");
        const auto got = normalized({start(0), set(1, .2), end(1), end(0)});
        expect_true(got == "S0 S1 =1 E1 E0", got);
    });

    Tests::add("stream: gestures carry across frames, and close_all ends what is open", [] {
        auto stream = Edit_stream{};
        auto out = std::vector<User_action>{};
        stream.normalize(std::vector{begin_tx(), start(2), set(3, .1)}, out);
        stream.normalize(std::vector{set(2, .4)}, out);
        stream.close_all(out);
        expect_true(describe(out) == "S S2 S3 =3 =2 E3 E2 E", describe(out));
        expect_true(stream.depth() == 0, "nothing left open");
    });
}

auto add_undo_tests() -> void
{
    Tests::add("undo: a drag over frames is one step, and the host sees one touch", [] {
        auto r = Rig{};
        r.push({start(0), set(0, .2)});
        r.push({set(0, .4)});
        r.push({set(0, .6), end(0)});
        expect_true(describe(r.host) == "S0 =0 =0 =0 E0", describe(r.host));
        expect_true(r.steps() == 1, "one step");
        r.undo();
        expect_true(r.params[0] == 0., "undo restores where it began");
    });

    Tests::add("undo: a follower set appended after a click's end joins its step (the frame amend window)", [] {
        auto r = Rig{};
        r.push({start(0), set(0, .5), end(0), set(1, .9)});
        expect_true(r.steps() == 1, std::format("one step, got {}", r.steps()));
        r.undo();
        expect_true(r.params[0] == 0. && r.params[1] == 0., "both revert together");
    });

    Tests::add("undo: copied start/end on a follower still groups, without sorting", [] {
        auto r = Rig{};
        r.push({start(0), set(0, .5), end(0), start(1), set(1, .9), end(1)});
        expect_true(r.steps() == 1, "one step");
    });

    Tests::add("undo: a gesture that ends and an unrelated one that begins in the same frame stay two steps", [] {
        auto r = Rig{};
        r.push({start(0), set(0, .5)});
        r.push({set(0, .6), end(0), start(1), set(1, .3)});
        r.push({set(1, .4), end(1)});
        expect_true(r.steps() == 2, std::format("two steps, got {}", r.steps()));
    });

    Tests::add("undo: two fingers on two knobs are one step, each released for the host on lift", [] {
        auto r = Rig{};
        r.push({start(0), start(1), set(0, .1), set(1, .2)});
        r.push({end(0)});
        expect_true(describe(r.host) == "S0 S1 =0 =1 E0", describe(r.host));
        r.push({set(1, .3), end(1)});
        expect_true(r.steps() == 1, "one step");
    });

    Tests::add("undo: a bare set is a step of its own", [] {
        auto r = Rig{};
        r.push({set(2, .7)});
        r.push({set(2, .8)});
        expect_true(r.steps() == 2, "a step each");
    });

    Tests::add("undo: a transaction groups params and a state source into one step", [] {
        auto r = Rig{};
        auto view = Fake_source{};
        view.bind(r.history);
        r.frame([&](Action_queue::Actor a) {
            r.history.actor().transaction([&] {
                a.push(set(0, .5));
                a.push(set(1, .25));
                view.value = std::byte{7};
            });
        });
        expect_true(describe(r.host) == "S0 =0 S1 =1 E0 E1", describe(r.host));
        expect_true(r.steps() == 1, "one step");
        r.undo();
        expect_true(r.params[0] == 0. && r.params[1] == 0. && view.value == std::byte{0}, "all of it reverts");
        r.redo();
        expect_true(r.params[0] == .5 && view.value == std::byte{7}, "and comes back");
    });

    Tests::add("undo: a transaction spans frames", [] {
        auto r = Rig{};
        auto view = Fake_source{};
        view.bind(r.history);
        r.frame([&](Action_queue::Actor a) { r.history.actor().begin(); a.push(set(0, .2)); });
        r.frame([&](Action_queue::Actor) { view.value = std::byte{3}; });
        expect_true(!r.history.can_undo(), "nothing to undo mid-transaction");
        r.frame([&](Action_queue::Actor a) { a.push(set(0, .4)); r.history.actor().end(); });
        expect_true(r.steps() == 1, "one step");
    });

    Tests::add("undo: begin() makes earlier uncommitted state edits a step of their own", [] {
        auto r = Rig{};
        auto view = Fake_source{};
        view.bind(r.history);
        r.frame([&](Action_queue::Actor a) {
            view.value = std::byte{1};
            r.history.actor().transaction([&] { a.push(set(0, .5)); view.value = std::byte{2}; });
        });
        expect_true(r.steps() == 2, std::format("two steps, got {}", r.steps()));
        r.undo();
        expect_true(view.value == std::byte{1} && r.params[0] == 0., "the transaction undoes alone");
    });

    Tests::add("undo: a transaction whose state is in flight is held until it confirms", [] {
        auto r = Rig{};
        auto doc = Fake_source{.source = Undo_history::Source::Document};
        doc.bind(r.history);
        r.frame([&](Action_queue::Actor a) {
            r.history.actor().transaction([&] { a.push(set(0, .5)); doc.value = std::byte{9}; doc.waiting = true; });
        });
        expect_true(!r.history.can_undo(), "held: nothing to undo yet");
        expect_true(r.history.holding(Undo_history::Source::Document), "holding the document");
        r.push({set(1, .3)}); // A step closing meanwhile keeps its place behind the held one.
        doc.confirm();
        expect_true(!r.history.holding(Undo_history::Source::Document) && r.history.can_undo(), "the step lands");
        r.undo();
        expect_true(r.params[1] == 0. && r.params[0] == .5, "the later step undoes first");
        r.undo();
        expect_true(r.params[0] == 0. && doc.value == std::byte{0}, "then the held one, params and document together");
    });

    Tests::add("undo: a held step whose edit was refused lands without it, and undo works again", [] {
        auto r = Rig{};
        auto doc = Fake_source{.source = Undo_history::Source::Document};
        doc.bind(r.history);
        r.frame([&](Action_queue::Actor a) {
            r.history.actor().transaction([&] { a.push(set(0, .5)); doc.value = std::byte{9}; doc.waiting = true; });
        });
        expect_true(r.history.holding(Undo_history::Source::Document), "held");
        doc.waiting = false;
        doc.value = doc.baseline; // The processor refused it: nothing to record.
        r.history.settle(Undo_history::Source::Document);
        expect_true(!r.history.holding(Undo_history::Source::Document) && r.history.can_undo(), "the step lands");
        r.undo();
        expect_true(r.params[0] == 0., "and undoes its params");
    });

    Tests::add("undo: a host load during a held step lands the held step first, below the load", [] {
        auto r = Rig{};
        auto doc = Fake_source{.source = Undo_history::Source::Document};
        doc.bind(r.history);
        r.frame([&](Action_queue::Actor a) {
            r.history.actor().transaction([&] { a.push(set(0, .5)); doc.value = std::byte{9}; doc.waiting = true; });
        });
        auto changes = std::vector<Set_param>{};
        const auto after = std::array<double, 4>{.5, .8, 0, 0};
        r.history.push_host_load(r.params, after, changes);
        r.params = after;
        doc.waiting = false; // The load reseeds the document, as Editor_link::load does.
        doc.baseline = doc.value;
        expect_true(!r.history.holding(Undo_history::Source::Document), "the load settles the hold");
        r.undo();
        expect_true(r.params[1] == 0. && r.params[0] == .5, "the first undo takes back the load");
        r.undo();
        expect_true(r.params[0] == 0., "the second the earlier edit");
    });

    Tests::add("undo: a state change in the frame a click closed joins its step", [] {
        auto r = Rig{};
        auto view = Fake_source{};
        view.bind(r.history);
        r.receiver.end_frame = [&] { view.commit(); }; // As Edit_store::end_frame commits the view.
        r.frame([&](Action_queue::Actor a) { a.push(set(0, .5)); view.value = std::byte{4}; });
        expect_true(r.steps() == 1, std::format("one step, got {}", r.steps()));
        r.undo();
        expect_true(r.params[0] == 0. && view.value == std::byte{0}, "both revert");
    });

    Tests::add("undo: an undo mid-gesture is dropped", [] {
        auto r = Rig{};
        r.push({set(0, .1)});
        r.push({start(1), set(1, .5)});
        r.history.actor().undo();
        r.frame();
        expect_true(r.params[0] == .1, "nothing undone while a gesture is open");
        r.push({end(1)});
        expect_true(r.steps() == 2, "both steps there");
    });

    Tests::add("undo: the last frame holds what resolved, replay included", [] {
        auto r = Rig{};
        r.push({set(0, .5)});
        expect_true(describe(r.actions.actor().last_frame()) == "S0 =0 E0", describe(r.actions.actor().last_frame()));
        r.undo();
        expect_true(describe(r.actions.actor().last_frame()) == "S0 =0 E0", "the undo's replay");
        expect_true(r.params[0] == 0., "and it reverted");
    });

    Tests::add("teardown: the view closing mid-drag releases the host and closes the step", [] {
        auto r = Rig{};
        r.frame([&](Action_queue::Actor a) { r.history.actor().begin(); a.push(start(0)); a.push(set(0, .5)); a.push(set(1, .2)); });
        r.host.clear();
        view_impl::close_session(r.receiver, r.params, r.actions, r.history);
        expect_true(describe(r.host) == "E1 E0", describe(r.host));
        expect_true(r.history.can_undo(), "the step closed");
        r.undo();
        expect_true(r.params[0] == 0. && r.params[1] == 0., "and undoes");
    });

    Tests::add("host loads: a load step takes its state too", [] {
        auto r = Rig{};
        auto before = std::array<double, 4>{};
        auto after = std::array<double, 4>{.5, 0, 0, 0};
        auto changes = std::vector<Set_param>{};
        r.history.push_host_load(before, after, changes);
        r.params = after;
        const auto from = std::byte{0}, to = std::byte{4};
        r.history.amend_host_state(Undo_history::Source::Document, &from, &to, 1);
        expect_true(r.steps() == 1, "one step");
    });
}

} // namespace

auto main() -> int
{
    add_stream_tests();
    add_undo_tests();
    return Tests::run_all() == 0 ? 0 : 1;
}
