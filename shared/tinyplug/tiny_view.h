#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <iostream>
#include <span>
#include <variant>

#include "tiny_events.h"
#include "tiny_queue.h"

class SkCanvas; // Skia canvas

namespace tiny {

// MARK: - Editor state
enum class State_tag : uint32_t {
    bool_ = 0, int_, double_, string_, bytes_
};
using State_item = std::variant<bool, int32_t, double, std::string, std::vector<uint8_t>>;

constexpr auto tag_for(const State_item& item) -> State_tag
{
    return std::visit(Inline_visitor{
        [](const bool&) { return State_tag::bool_; },
        [](const int32_t&) { return State_tag::int_; },
        [](const double&) { return State_tag::double_; },
        [](const std::string&) { return State_tag::string_; },
        [](const std::vector<uint8_t>&) { return State_tag::bytes_; },
    }, item);
}

using State_map = std::unordered_map<std::string, State_item>;

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

// MARK: - pointer states

// The pointer state either has been consumed, or is outside the window. The "nothing" state.
struct Consumed {
    bool operator==(const Consumed&) const = default;
};

// The pointer is over `pos` but not pressed.
struct Over {
    Coords pos{};
    bool operator==(const Over&) const = default;
};

// The pointer went down at `pos`. Sent once.
struct Down {
    Coords pos{}; bool right{};
    bool operator==(const Down&) const = default;
};

// The pointer has been over `pos` for 2000 ms. Sent once.
struct Dwell {
    Coords pos{};
    bool operator==(const Dwell&) const = default;
};

// The pointer has clicked (or tapped) at `pos`.
struct Click {
    Coords pos{};
    bool operator==(const Click&) const = default;
};

// The pointer has double clicked (or tapped) at `pos`. May or may not be preceded by `Click`.
struct Double_click {
    Coords pos{};
    bool operator==(const Double_click&) const = default;
};

// The pointer has started a drag. `fPos` will remain stable across the drag.
struct Drag_start {
    Coords fpos{}; Coords tpos{};
    bool operator==(const Drag_start&) const = default;
};

// The pointer has continued a drag. `fPos` is thep osition where the drag started.
struct Drag {
    Coords fpos{}; Coords tpos{};
    bool operator==(const Drag&) const = default;
};

// The pointer has ended a drag. `fPos` is the position where the drag started.
struct Drag_end {
    Coords fpos{}; Coords tpos{};
    bool operator==(const Drag_end&) const = default;
};

// The pointer has right clicked at `pos`, for touch interfaces this will be a long press.
struct Right_click {
    Coords pos{};
    bool operator==(const Right_click&) const = default;
};

// The state of the user's pointer (a mouse or a finger).
using Pointer_state = std::variant<Consumed, Over, Down, Dwell, Click, Double_click, Drag_start, Drag, Drag_end, Right_click>;

struct Pointer {
    uintptr_t tag{};
    Pointer_state state{};
    bool operator==(const Pointer&) const = default;
};

// Try to set the pointer state. Returns whether or not the pointer state was set.
// - You can always consume the pointer state.
// - You can only set a new pointer state if it has already been consumed.
// - Custom UI elements should only consume the pointer state.
inline auto try_set(Pointer_state& old_state, Pointer_state new_state) -> bool
{
    // You can always consume the pointer state.
    if (std::holds_alternative<Consumed>(new_state)) {
        old_state = new_state;
        return true;
    }
    // You can only set a new pointer state if it has already been consumed.
    else if (std::holds_alternative<Consumed>(old_state)) {
        old_state = new_state;
        return true;
    }
    return false;
}

// Since down is transient, we need to be able to track it.
inline auto track_is_down(const Pointer_state& state, bool& is_down)
{
    const auto implies_down = std::holds_alternative<Down>(state) || std::holds_alternative<Drag_start>(state) || std::holds_alternative<Drag>(state);
    const auto implies_up = !(implies_down || std::holds_alternative<Consumed>(state));
    if (implies_down) {
        is_down = true;
    }
    else if (implies_up) {
        is_down = false;
    }
}

// Controls can only consume the pointer state if it originates in their frame.
inline auto try_consume(Pointer_state& state, const Frame& frame) -> std::optional<Pointer_state>
{
    using Opt = std::optional<Pointer_state>;
    auto get_pos = [](const auto& s) -> Coords {
        if constexpr (requires { s.fpos; }) return s.fpos;
        else return s.pos;
    };
    return std::visit(Inline_visitor{
        [&](const Consumed&) { return Opt{}; },
        [&](const auto& s) {
            if (frame.contains(get_pos(s))) {
                state = Consumed{};
                return Opt{s};
            }
            return Opt{};
        },
    }, state);
}

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

// MARK: - user interaction

// A user interaction includes an id (for future multi-touch), pointer state, and scroll deltas.
struct User_interaction {
    std::vector<Pointer> pointers{};
    Coords scroll_deltas{};
    Modifier_keys modifier_keys{};
    bool operator==(const User_interaction&) const = default;
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
    bool operator==(const View_context&) const = default;
};

// Read-only access to some param (and export) values.
struct Processor_state {
    std::span<const double> params{};
    std::span<const double> meters{};
};

// The app state gives you
// - Read-only access to the param and meter values.
// - A view context with the interaction state and a canvas in which to draw.
struct App_state {
    Processor_state processor_state{};
    View_context view_context{};
};

// The plug-in format's view will have a draw callback.
// This is where it will resolve the application state and pass it to your custom view.
using Draw_callback = std::function<void(View_context&)>;
using Notify_callback = std::function<void(const Ui_notification&)>;

// MARK: action queue

struct Action_queue {

    using Actions = std::vector<User_action>;

    struct Receiver {
        explicit Receiver(Actions* actions = nullptr) : _actions{actions} {}
        auto push(const User_action& action) const -> void
        {
            if (_actions) {
                _actions->push_back(action);
            }
        }
    private:
        Actions* _actions;
    };

    auto make_receiver() -> Receiver
    {
        return Receiver{&_actions};
    }

    auto actions() const -> const Actions&
    {
        return _actions;
    }

    auto clear() -> void
    {
        _actions.clear();
    }

private:

    Actions _actions = Actions(16); // initial size

};

// MARK: - task queue

struct Task_queue {
    using Task = std::function<void()>;
    using Queue = Lock_free_queue<Task, 16>;

    struct Receiver {
        explicit Receiver(Queue* queue = nullptr) : _queue{queue} {}
        auto push(Task task) const -> void
        {
            if (_queue) {
                [[maybe_unused]] const auto success = _queue->push(std::move(task));
                assert(success && "Queue not big enough!");
            }
        }
    private:
        Queue* _queue{nullptr};
    };

    auto make_receiver() -> Receiver
    {
        return Receiver{&_queue};
    }

    auto execute_all() -> void
    {
        auto task = Task{};
        while (_queue.pop(task)) task();
    }

private:

    Queue _queue{};

};

template<typename... Args>
struct Later {

    std::function<void(Args...)> callback{[](Args...) {}};
    std::optional<Task_queue::Receiver> receiver{};

    template<typename... Cargs>
    void operator()(Cargs&&... args) const {
        if (receiver) {
            receiver->push([cb = callback, ...a = std::forward<Cargs>(args)]() {
                cb(std::move(a)...);
            });
        } else {
            callback(std::forward<Cargs>(args)...);
        }
    }
};

struct View_connection {
    Action_queue::Receiver actions;
    Task_queue::Receiver tasks; // Maybe we don't need this any more.
};

// MARK: - debug

// Debug pointer state.
inline auto print_pointer_state(const Pointer_state& state) -> void
{
    std::visit(Inline_visitor{
        [](const Consumed&) {/* skip */},
        [](const Over& s) {
            std::cout << "Over at (" << s.pos.x << ", " << s.pos.y << ")\n";
        },
        [](const Down& s) {
            std::cout << "Down at (" << s.pos.x << ", " << s.pos.y << ")\n";
        },
        [](const Dwell& s) {
            std::cout << "Dwell at (" << s.pos.x << ", " << s.pos.y << ")\n";
        },
        [](const Click& s) {
            std::cout << "Click at (" << s.pos.x << ", " << s.pos.y << ")\n";
        },
        [](const Double_click& s) {
            std::cout << "Double_click at (" << s.pos.x << ", " << s.pos.y << ")\n";
        },
        [](const Drag_start& s) {
            std::cout << "Drag_start from (" << s.fpos.x << ", " << s.fpos.y
                      << ") to (" << s.tpos.x << ", " << s.tpos.y << ")\n";
        },
        [](const Drag& s) {
            std::cout << "Drag from (" << s.fpos.x << ", " << s.fpos.y
                      << ") to (" << s.tpos.x << ", " << s.tpos.y << ")\n";
        },
        [](const Drag_end& s) {
            std::cout << "Drag_end from (" << s.fpos.x << ", " << s.fpos.y
                      << ") to (" << s.tpos.x << ", " << s.tpos.y << ")\n";
        },
        [](const Right_click& s) {
            std::cout << "Right_click at (" << s.pos.x << ", " << s.pos.y << ")\n";
        }
    }, state);
}

namespace view_impl {

// MARK: - run_frame

template<typename M, typename S, typename A0, typename A1, typename C, typename V, typename A, typename T>
constexpr auto run_frame(
    const M& _meter_infos,
    const S& _receiver,
    A0& _ui_params, 
    A1& _ui_meters, 
    const C& view_context, 
    V* _custom_view,
    A& _actions,
    T& _tasks
) -> void
{
    // Pop the exports.
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                _ui_params[p.id] = p.value;
            },
            [&](const Set_meter& e) {
                //
                auto& uiexport = _ui_meters[e.id];
                const auto type = _meter_infos.policy_for(e.id);

                using enum Meter_policy;
                switch (type) {
                    case peak: {
                        if (!uiexport.updated) {
                            uiexport.value = 0; // Reset on first update in frame where we receive an event.
                        }
                        uiexport.value = std::max(uiexport.value, e.value);
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
    auto meter_arr = std::array<double, M::num_meters>{};
    const auto value_tx = _ui_meters | std::views::transform(&Tagged_meter::value);
    std::ranges::copy(value_tx, meter_arr.begin());

    // Create view context.
    auto app_state = App_state{
        .processor_state = {_ui_params, meter_arr},
        .view_context = view_context,
    };
    _actions.clear(); // Actually clear before we draw.

    // Tell the user view to draw.
    _custom_view->on_gui_draw(app_state);

    _tasks.execute_all(); // Execute laters (might push into actions).

    // Handle actions and update local state.
    for (auto& action : _actions.actions()) {
        _receiver.action_handler(action);
        if (const auto* s = std::get_if<Set_param>(&action)) {
            _ui_params[s->id] = s->value; // Update the local copy.
        }
    }
    //_actions.clear();

    // Reset meters for next frame.
    for (auto& ui_meter : _ui_meters) {
        ui_meter.updated = false;
        if (ui_meter.trigged) {
            ui_meter.value = 0;
            ui_meter.trigged = false;
        }
    }
}

} // namespace view_impl

} // namespace tiny