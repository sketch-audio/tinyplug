// `Edit_store`: the view model and the window size as undo sources, host loads reported to the view
// model's `on_host_load`, and the editor's record saved and restored. Against a stand-in with a view
// model (support/view_models), through `view_impl::resolve_frame` as the wrappers run it.

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tinyplug/tiny_view.hpp>

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using namespace tiny;

struct Rig {
    Action_queue actions{};
    Undo_history history{};
    Edit_store store;
    std::array<double, 2> params{};
    std::vector<Rect_size> applied{};
    Ui_receiver receiver{};

    explicit Rig(Sizing sizing = {.preferred = {400, 300}}) : store{sizing}
    {
        history.bind_actions(actions.actor());
        store.bind(history);
        store.set_apply([this](Rect_size s) { applied.push_back(s); });
        receiver.end_frame = [this] { store.end_frame(); };
    }

    auto frame(const std::function<void(Action_queue::Actor)>& draw = {}) -> void
    {
        actions.clear();
        if (draw) draw(actions.actor());
        auto state = Processor_state{};
        state.params = params;
        view_impl::resolve_frame(receiver, params, actions, history, state, [](auto, auto) {});
    }

    auto view() -> Edit_store::Handle { return store.handle(); }
    auto undo() -> void { history.actor().undo(); frame(); }
    auto redo() -> void { history.actor().redo(); frame(); }

    // A host load as the wrappers run one: params into the step, then the editor's half.
    auto host_load(std::array<double, 2> after, State_map legacy, const std::optional<Edit_record>& record, Load_info info = {}) -> void
    {
        auto changes = std::vector<Set_param>{};
        history.push_host_load(params, after, changes);
        params = after;
        load_editor_state(store, std::move(legacy), record, params, info);
    }
};

auto name(Rig& r) -> std::string { return std::string{r.view().get().preset_name.view()}; }

auto add_tests() -> void
{
    Tests::add("view: an edit is a step at the frame's end; the restored part never is", [] {
        auto r = Rig{};
        r.frame([&](auto) { r.view().edit([](auto& v) { v.undoable.b_selected = 1; }); });
        r.frame([&](auto) { r.view().restored().tab = 3; });
        expect_true(r.history.can_undo(), "one step");
        r.undo();
        expect_true(r.view().get().b_selected == 0, "undo reverts the selection");
        expect_true(r.view().restored().tab == 3, "and leaves the tab alone");
        expect_true(!r.history.can_undo(), "the tab was never a step");
    });

    Tests::add("view: an edit inside a transaction joins its params", [] {
        auto r = Rig{};
        r.frame([&](Action_queue::Actor a) {
            r.history.actor().transaction([&] {
                a.push(Set_param{0, .7});
                r.view().edit([](auto& v) { v.undoable.b_selected = 1; });
            });
        });
        r.undo();
        expect_true(r.params[0] == 0. && r.view().get().b_selected == 0, "one undo reverts both");
        expect_true(!r.history.can_undo(), "it was one step");
    });

    Tests::add("loads: a session restores the view in the load's own step", [] {
        auto r = Rig{};
        auto saved = Rig{};
        saved.frame([&](auto) { saved.view().edit([](auto& v) { v.undoable.preset_name = "Warm"; v.undoable.b_selected = 1; }); });
        saved.frame([&](auto) { saved.view().restored().tab = 2; });
        const auto record = saved.store.save();

        r.host_load({.5, .5}, {}, record);
        expect_true(name(r) == "Warm" && r.view().get().b_selected == 1 && r.view().restored().tab == 2, "all of it restored");
        expect_true(r.view().loads() == 1, "the load is counted");
        r.undo();
        expect_true(r.params[0] == 0. && name(r).empty() && r.view().get().b_selected == 0, "one undo reverts params and view");
        expect_true(r.view().restored().tab == 2, "but not the tab");
    });

    Tests::add("loads: a preset keeps the view and tells the hook its name", [] {
        auto r = Rig{};
        r.frame([&](auto) { r.view().edit([](auto& v) { v.undoable.b_selected = 1; }); });
        r.host_load({.3, .3}, {}, std::nullopt, Load_info{.kind = Load_info::Kind::Preset, .name = "Bright"});
        expect_true(name(r) == "Bright", "the hook named it");
        expect_true(r.view().get().b_selected == 1, "and the A/B selection stayed");
        r.undo();
        expect_true(name(r).empty() && r.params[0] == 0., "undo reverts the name with the values");
    });

    Tests::add("loads: a native preset's framework keys make it a preset", [] {
        auto r = Rig{};
        auto legacy = State_map{};
        framework_keys::write(legacy, Framework_keys{.preset_name = "From TFX"});
        r.host_load({.3, .3}, legacy, std::nullopt);
        expect_true(name(r) == "From TFX", name(r));
    });

    Tests::add("loads: an older session's map reaches the hook, and its size keys the store", [] {
        auto r = Rig{};
        r.frame([&](auto) { r.view().edit([](auto& v) { v.undoable.b_selected = 1; }); });
        auto legacy = State_map{{"preset-name", std::string{"Old Session"}}};
        editor_size_state::inject(legacy, 613, 411);
        r.host_load({.1, .1}, legacy, std::nullopt);
        expect_true(name(r) == "Old Session", "the author's key, mapped by the hook");
        expect_true(r.view().get().b_selected == 0, "the rest as a new instance holds it");
        expect_true(r.store.size() == Rect_size{613, 411}, "the size from the framework's keys");
        expect_true(r.store.save().size == Rect_size{613, 411}, "and saved from now on");
    });

    Tests::add("saves: the record round-trips, from any thread", [] {
        auto r = Rig{};
        r.frame([&](auto) { r.view().edit([](auto& v) { v.undoable.preset_name = "Saved"; }); });
        r.store.resized({500, 350});
        r.frame();
        auto record = Edit_record{};
        std::thread{[&] { record = r.store.save(); }}.join();
        const auto bytes = encode_edit_record(record);
        const auto decoded = decode_edit_record(bytes);
        expect_true(decoded && *decoded == record, "encode and decode");
        auto other = Rig{};
        other.host_load({0, 0}, {}, decoded);
        expect_true(name(other) == "Saved" && other.store.size() == Rect_size{500, 350}, "and it loads");
        expect_true(legacy_editor_map(other.store).size() == 2, "older builds get the size keys");
    });

    Tests::add("size: a resize inside a transaction is a step when sizing says so", [] {
        auto r = Rig{Sizing{.preferred = {400, 300}, .min = {200, 150}, .undoable = true}};
        r.frame([&](auto) { r.history.actor().begin(); r.store.requested({600, 450}); });
        r.frame([&](auto) { r.store.requested({640, 480}); r.history.actor().end(); });
        expect_true(r.history.can_undo(), "one step");
        r.undo();
        expect_true(r.store.size() == Rect_size{400, 300}, "undo restores the size");
        expect_true(!r.applied.empty() && r.applied.back() == Rect_size{400, 300}, "and asks the window to follow");
        r.store.resized({700, 500}); // The host's frame drag.
        r.frame();
        expect_true(!r.history.can_undo(), "a host-driven resize is never a step");
        expect_true(r.store.sizing().clamp({10, 10}) == Rect_size{200, 150}, "limits clamp");
    });

    Tests::add("size: without sizing().undoable a resize is never a step", [] {
        auto r = Rig{};
        r.frame([&](auto) { r.history.actor().transaction([&] { r.store.requested({600, 450}); }); });
        expect_true(!r.history.can_undo(), "no step");
        expect_true(r.store.size() == Rect_size{600, 450}, "but the size is kept");
    });

    Tests::add("handle: generation moves with every change to the undoable part", [] {
        auto r = Rig{};
        const auto g0 = r.view().generation();
        r.frame([&](auto) { r.view().edit([](auto& v) { v.undoable.b_selected = 1; }); });
        const auto g1 = r.view().generation();
        r.undo();
        expect_true(g1 > g0 && r.view().generation() > g1, "edit and undo both bump it");
        expect_true(!r.view().edit([](auto& v) { v.undoable.b_selected = 0; }), "an edit that changes nothing is no edit");
    });
}

} // namespace

auto main() -> int
{
    add_tests();
    return Tests::run_all() == 0 ? 0 : 1;
}
