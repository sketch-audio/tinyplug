#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <functional>
#include <span>
#include <variant>

#include <tiny_core/lock_free_queue.hpp>
#include <tiny_core/tiny_input.hpp>
#include <tiny_core/tiny_meters.hpp>
#include <tiny_core/tiny_params.hpp>
#include <tiny_core/tiny_utils.hpp>

#include "tiny_edit.hpp"
#include "tiny_events.hpp"

namespace tiny {

struct Processor_view {
    std::span<const double> param_values{};
    std::span<const double> meter_values{};
};

struct Update_context {
    Edit_context edit{}; // The edit context is not immediate mode so you need to attach it in your update calls.
    Modifier_keys modifier_keys{};
    Scroll_data scroll_data{};
};

// The app state gives you
// - Read-only access to the param and meter values.
// - A view context with the interaction state and a canvas in which to draw.
struct Plugin_state {
    Processor_state processor_state{};
    View_context view_context{};
};

// MARK: - debug

namespace view_impl {

// MARK: - resolve_frame

// The frame's actions are queued: give every set a gesture, record undo, run a deferred undo or redo,
// hand each action to the wrapper, and keep them as the frame's resolved edits. Wrappers never see a
// transaction's own start and end: those are the undo history's.
template<typename S, typename P, typename A, typename U, typename F>
inline auto resolve_frame(const S& _receiver, P& _ui_params, A& _actions, U& _undo_history, Processor_state& state, F _resize_policy) -> void
{
    _actions.normalize();
    _undo_history.process_actions(_actions.get_actions(), state);
    _undo_history.perform_actions(_actions.actor()); // Pushes the replay of an undo or redo.
    _actions.normalize();

    const auto actions = _actions.get_actions(); // A copy: a handler may push.
    for (const auto& action : actions) {
        if (const auto* s = std::get_if<Action_start>(&action); s && !s->address) continue;
        if (const auto* e = std::get_if<Action_end>(&action); e && !e->address) continue;
        _receiver.action_handler(action);
        if (const auto* s = std::get_if<Set_param>(&action)) {
            _ui_params[s->address] = s->value; // Update the local copy.
        }
        else if (const auto* r = std::get_if<Request_resize>(&action)) {
            _resize_policy(r->width, r->height);
        }
    }

    _actions.finish_frame();
    _actions.clear();

#if TINY_HAS_STATE
    _receiver.sync_state(); // Send this frame's edits and any undo/redo replay.
#endif
    _receiver.end_frame();
    _undo_history.end_frame();
}

// MARK: - close_session

// The editor's view is going away. What it pushed since its last frame (a hide that cancels a drag)
// resolves now, and whatever is still open ends: no parameter stays touched for the host, and the
// undo history never stays mid-gesture.
template<typename S, typename P, typename A, typename U>
inline auto close_session(const S& _receiver, P& _ui_params, A& _actions, U& _undo_history) -> void
{
    _actions.close_all();
    auto state = Processor_state{};
    state.params = _ui_params;
    resolve_frame(_receiver, _ui_params, _actions, _undo_history, state, [](auto, auto) {});
}

// MARK: - run_frame

template<typename S, typename A0, typename A1, typename B, typename C, typename V, typename A, typename U, typename T, typename F>
inline auto run_frame(
    const S& _receiver,
    A0& _ui_params, 
    A1& _ui_meters, 
    B& _ui_blocks,
    const C& view_context, 
    V* _custom_view,
    A& _actions,
    U& _undo_history,
    T& _tasks,
    F _resize_policy
) -> void
{
    _tasks.bind_main(std::this_thread::get_id());

#if TINY_HAS_STATE
    _receiver.sync_state();
#endif

    if constexpr (has_meters) {
        auto values = std::array<float, std::tuple_size_v<A1>>{};
        _receiver.read_meters(values);
        std::ranges::copy(values, _ui_meters.begin());
    }

    // Create view context.
    auto state = Plugin_state{.view_context = view_context};
    state.processor_state.params = _ui_params;
#if TINY_HAS_METERS
    state.processor_state.meters = _ui_meters;
#endif
#if TINY_HAS_BLOCKS
    _receiver.read_blocks(_ui_blocks);
    state.processor_state.blocks = decltype(state.processor_state.blocks){&_ui_blocks};
#else
    (void)_ui_blocks;
#endif
    _actions.clear(); // Actually clear before we draw.

    // Tell the user view to draw.
    _tasks.run_main();
    _custom_view->on_gui_draw(state);

    resolve_frame(_receiver, _ui_params, _actions, _undo_history, state.processor_state, _resize_policy);
}

} // namespace view_impl

} // namespace tiny
