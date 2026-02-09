#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <variant>

#include "../platform/platform.h"

#include "tiny_edit.h"
#include "tiny_events.h"
#include "tiny_meters.h"
#include "tiny_params.h"
#include "lock_free_queue.hpp"
#include "tiny_utils.h"

class SkCanvas; // Skia canvas

namespace tiny {

// MARK: - helpers

struct Coords {
    double x{}; double y{};
    bool operator==(const Coords&) const = default;

    auto relative_to(const Coords& p) const -> Coords
    {
        return {x - p.x, y - p.y};
    }
};

struct Frame {
    double x{}; double y{}; double w{}; double h{};
    bool operator==(const Frame&) const = default;

    auto contains(const Coords& p) const -> bool
    {
        return x <= p.x && p.x <= (x + w) && y <= p.y && p.y <= (y + h);
    }
};

struct Rect_size {
    int32_t w{};  int32_t h{};
    bool operator==(const Rect_size&) const = default;
};

struct Modifier_keys {
    // The primary platform modifier: Ctrl (Windows), command (Apple).
    bool primary{};

    // The alternate modifier: Alt (Windows), option (Apple).
    bool alt{};

    // The shift key.
    bool shift{};

    auto any() -> bool
    {
        return primary || alt || shift;
    }
    
    // Regular.
    bool operator==(const Modifier_keys&) const = default;
};

// MARK: - notifications

// Ideas:
// - view resized (atm passed with View_context)
// - dpi changed (atm passed with View_context)

struct Dark_mode_changed {
    bool new_value{};
};

using Ui_notification = std::variant<Dark_mode_changed>;

// MARK: - new gestures

using Steady_clock = std::chrono::steady_clock;
using Steady_time = std::chrono::time_point<Steady_clock>;

enum class Pointer_button : uint32_t { left, right }; // Maybe add middle some day.

struct Pointer_down {
    Pointer_button button{};
    Coords pos{};
};

struct Pointer_up {
    Pointer_button button{};
    Coords pos{};
};

struct Pointer_move {
    Coords pos{};
};

struct Pointer_click {
    Pointer_button button{};
    uint32_t count{}; // Up to 2 for now.
    Coords pos{};
};

struct Pointer_enter {
    Coords pos{};
};

struct Pointer_exit {
    Coords pos{};
};

struct Pointer_cancel {
    Coords pos{};
};

using Pointer_event = std::variant<Pointer_down, Pointer_up, Pointer_move, Pointer_click, Pointer_enter, Pointer_exit, Pointer_cancel>;

struct Event {
    Pointer_event event{};
    uintptr_t pointer_tag{};
    bool consumed{};
};

struct Event_list {
    std::vector<Event> events{}; // All events in the list can be considered simultaneous.
    Steady_time timestamp{};

    auto events_in(const Frame& frame) -> Event_list
    {
        auto filtered = std::vector<Event>{};
        for (const auto& event : events) {
            std::visit([&](const auto& e) {
                if constexpr (requires { e.pos; }) {
                    if (frame.contains(e.pos)) {
                        filtered.push_back(event);
                    }
                }
            }, event.event);
        }
        return {filtered, timestamp};
    }
};

struct Event_stream {

    auto push(const Event& event) -> void
    {
        events.push_back(event);
    }

    auto consume(Steady_time timestamp) -> Event_list
    {
        const auto list = Event_list{consolidate_moves(events), timestamp};
        events.clear();
        return list;
    }

private:

    std::vector<Event> events{};

    auto consolidate_moves(const std::vector<Event>& es) -> std::vector<Event>
    {
        auto out_rev = std::vector<Event>{};
        auto seen = std::unordered_set<uintptr_t>{};
        for (const auto& e : std::ranges::reverse_view(es)) {
            std::visit(Inline_visitor{
                [&](const Pointer_move&) {
                    if (seen.find(e.pointer_tag) == seen.end()) {
                        out_rev.push_back(e);
                        seen.insert(e.pointer_tag);
                    }
                },
                [&](const auto&) {
                    out_rev.push_back(e);
                }
            }, e.event);
        }
        std::ranges::reverse(out_rev);
        return out_rev;
    }

};

// MARK: - user interaction

// A user interaction includes an id (for future multi-touch), pointer state, and scroll deltas.
struct User_interaction {
    Event_list events{};
    Coords scroll_deltas{};
    bool inertial_scroll{};
    Modifier_keys modifier_keys{};
    //bool operator==(const User_interaction&) const = default;
};

// MARK: - time

using System_clock = std::chrono::system_clock;
using Time_point = std::chrono::time_point<System_clock>;
struct Durations {
    static auto delta_secs(Time_point ti, Time_point tf) -> double
    {
        return std::chrono::duration<double>(tf - ti).count();
    }
};

// MARK: - app state

// The view context consists of the current time, user interaction(s), a Skia canvas, a logical size, and scale.
struct View_context {
    Time_point time_now{};
    User_interaction interaction{};
    SkCanvas* canvas{nullptr};
    Rect_size logical_size{};
    double scale{1};
    //bool operator==(const View_context&) const = default;
};

struct Scroll_data {
    Coords deltas{};
    bool inertial{};
};

struct Processor_view {
    std::span<const double> param_values{};
    std::span<const double> meter_values{};
};

struct Update_context {
    Edit_context edit{}; // The edit context is not immediate mode so you need to attach it in your update calls.
    Modifier_keys modifier_keys{};
    Scroll_data scroll_data{};
};

struct Draw_context {
    SkCanvas* canvas{};
    Rect_size logical_size{};
    double scale{1};
    Time_point time_now{};
};

// The app state gives you
// - Read-only access to the param and meter values.
// - A view context with the interaction state and a canvas in which to draw.
struct Plugin_state {
    Processor_state processor_state{};
    View_context view_context{};
};


// The plug-in format's view will have a draw callback.
// This is where it will resolve the application state and pass it to your custom view.
using Draw_callback = std::function<void(View_context&)>;
using Notify_callback = std::function<void(const Ui_notification&)>;

// MARK: - debug

namespace view_impl {

// MARK: - run_frame

template<typename M, typename S, typename A0, typename A1, typename C, typename V, typename A, typename U, typename T>
constexpr auto run_frame(
    const M& _meter_specs,
    const S& _receiver,
    A0& _ui_params, 
    A1& _ui_meters, 
    const C& view_context, 
    V* _custom_view,
    A& _actions,
    U& _undo_history,
    T& _tasks
) -> void
{
    _tasks.bind_main(std::this_thread::get_id());

    // Pop the exports.
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                _ui_params[p.address] = p.value;
            },
            [&](const Set_meter& e) {
                //
                auto& uiexport = _ui_meters[e.address];
                const auto type = _meter_specs[e.address].policy;

                using enum Meter_policy;
                switch (type) {
                    case peak: {
                        if (!uiexport.updated) {
                            uiexport.value = 0; // Reset on first update in frame where we receive an event.
                        }
                        uiexport.value = std::max(uiexport.value, e.value);
                        uiexport.last_is_zero = (e.value == 0.f);
                        uiexport.updated = true;
                        break;
                    }
                    case stream: {
                        uiexport.value = e.value;
                        uiexport.updated = true;
                        break;
                    }
                    case trig: {
                        uiexport.value = 1;
                        uiexport.trigged = true;
                        break;
                    }
                }
            }
        }, event);
    }

    // Adapt tagged meters to values.
    auto meter_arr = std::vector<double>{};
    meter_arr.resize(_meter_specs.size());
    const auto value_tx = _ui_meters | std::views::transform(&Tagged_meter::value);
    std::ranges::copy(value_tx, meter_arr.begin());

    // Create view context.
    auto state = Plugin_state{
        .processor_state = {_ui_params, meter_arr},
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
    for (auto& action : _actions.get_actions()) {
        _receiver.action_handler(action);
        if (const auto* s = std::get_if<Set_param>(&action)) {
            _ui_params[s->address] = s->value; // Update the local copy.
        }
    }

    _actions.process_observers(_ui_params); // Use manifested state.

    // Reset meters for next frame.
    for (auto& ui_meter : _ui_meters) {
        ui_meter.updated = false;
        if (ui_meter.trigged) {
            ui_meter.value = 0;
            ui_meter.trigged = false;
        }
        if (ui_meter.last_is_zero) {
            ui_meter.value = 0;
            ui_meter.last_is_zero = false;
        }
    }
}

} // namespace view_impl

} // namespace tiny
