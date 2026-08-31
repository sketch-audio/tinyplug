#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <unordered_set>
#include <variant>

#include "tiny_edit.hpp"
#include "tiny_events.hpp"
#include "tiny_meters.hpp"
#include "tiny_params.hpp"
#include "lock_free_queue.hpp"
#include "tiny_utils.hpp"

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
//
// Dark_mode_changed now lives in tiny_events.hpp as part of Host_event, the single
// event type delivered to the editor's notify().

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

struct Event_origin {
    Coords pos{};
    uintptr_t tag{};
    bool operator==(const Event_origin&) const = default;

    struct Hasher {
        auto operator()(const Event_origin& origin) const -> size_t
        {
            return std::hash<uintptr_t>{}(origin.tag);
        }
    };
};


struct Event_list {
    std::vector<Event> events{}; // All events in the list can be considered simultaneous.
    std::unordered_set<Event_origin, Event_origin::Hasher> pointer_origins{}; // The held pointer tags.
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
        return {filtered, pointer_origins, timestamp};
    }
};

struct Event_stream {

    auto push(const Event& event) -> void
    {
        events.push_back(event);

        if (const auto* down = std::get_if<Pointer_down>(&event.event)) {
            pointer_origins.insert({down->pos, event.pointer_tag});
        }
        else if (std::holds_alternative<Pointer_up>(event.event) || std::holds_alternative<Pointer_cancel>(event.event)) {
            erase_origin(event.pointer_tag);
        }
    }

    auto consume(Steady_time timestamp) -> Event_list
    {
        const auto list = Event_list{consolidate_moves(events), pointer_origins, timestamp};
        events.clear();
        return list;
    }

private:

    std::vector<Event> events{};
    std::unordered_set<Event_origin, Event_origin::Hasher> pointer_origins{};

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

    auto erase_origin(uintptr_t tag) -> void
    {
        // Find origin with tag and erase.
        for (auto it = pointer_origins.begin(); it != pointer_origins.end(); ++it) {
            if (it->tag == tag) {
                pointer_origins.erase(it);
                break;
            }
        }
    }

};

// MARK: - user interaction

// A user interaction includes an id (for future multi-touch), pointer state, and scroll deltas.
struct User_interaction {
    Event_list events{};
    Coords scroll_deltas{};
    bool inertial_scroll{};
    bool precise_scroll{}; // Trackpad/precision device: deltas are points of pointer travel.
    Modifier_keys modifier_keys{};
    Coords pointer_abs{}; // Pointer absolute position, useful for live resize.
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
using Notify_callback = std::function<void(const Dark_mode_changed&)>;

// MARK: - debug

namespace view_impl {

// MARK: - run_frame

template<typename M, typename S, typename A0, typename A1, typename C, typename V, typename A, typename U, typename T, typename F>
inline auto run_frame(
    const M& _meter_specs,
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

    // Read the meter mailbox: one pass, one sample per address. The old drain loop
    // had to re-derive per-frame coalescing (max for a peak, latest for a level, a
    // one-frame flag for an event) from a stream of individual values; the mailbox
    // combines on the way in, so all that bookkeeping — and the `Meter_state` it
    // lived in — is gone.
    auto samples = std::array<meters::Sample, std::tuple_size_v<A1>>{};
    _receiver.read_meters(samples);

    for (auto i = size_t{}; i < samples.size(); ++i) {
        // A level and a peak are both just "the value"; the mailbox already decided
        // what that means for each. An event shows its magnitude on the frames it
        // actually fired, and nothing on the others.
        _ui_meters[i] = (_meter_specs[i].policy == meters::Policy::Trig)
            ? (samples[i].triggers > 0 ? static_cast<double>(samples[i].value) : 0.)
            : static_cast<double>(samples[i].value);
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
