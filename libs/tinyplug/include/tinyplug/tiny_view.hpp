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

// MARK: - run_frame

template<typename S, typename A0, typename A1, typename C, typename V, typename A, typename U, typename T, typename F>
inline auto run_frame(
    const S& _receiver,
    A0& _ui_params, 
    A1& _ui_meters, 
    const C& view_context, 
    V* _custom_view,
    A& _actions,
    U& _undo_history,
    T& _tasks,
    F _resize_policy
) -> void
{
    _tasks.bind_main(std::this_thread::get_id());

    if constexpr (has_meters) {
        auto values = std::array<float, std::tuple_size_v<A1>>{};
        _receiver.read_meters(values);
        std::ranges::copy(values, _ui_meters.begin());
    }

    // Create view context.
    auto state = Plugin_state{
        .processor_state = {_ui_params, _ui_meters},
        .view_context = view_context,
    };
    _actions.clear(); // Actually clear before we draw.

    // Tell the user view to draw.
    _tasks.run_main();
    _custom_view->on_gui_draw(state);

    // Observe actions for undo/redo.
    _undo_history.process_actions(_actions.get_actions(), state.processor_state);

    // Process deferred undo/redo actions (does the actual undo/redo and pushes into actions).
    _undo_history.perform_actions(_actions.actor());

    // Handle actions and update local state.

    // Grab value.
    const auto actions = _actions.get_actions();
    for (const auto& action : actions) {
        _receiver.action_handler(action);
        if (const auto* s = std::get_if<Set_param>(&action)) {
            _ui_params[s->address] = s->value; // Update the local copy.
        }
        else if (const auto* r = std::get_if<Request_resize>(&action)) {
            _resize_policy(r->width, r->height);
        }
    }

    _actions.process_observers(_ui_params); // Use manifested state.
}

} // namespace view_impl

} // namespace tiny
