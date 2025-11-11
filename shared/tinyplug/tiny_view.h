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
    // You can override down with drag start
    else if (std::holds_alternative<Down>(old_state) && std::holds_alternative<Drag_start>(new_state)) {
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

inline auto track_over(const Pointer_state& state, const Frame& frame, bool& over) -> void
{
    auto get_pos = [](const auto& s) -> Coords {
        if constexpr (requires { s.fpos; }) return s.fpos;
        else return s.pos;
    };
    std::visit(Inline_visitor{
        [&](const Consumed&) {}, // Consumed no change.
        [&](const auto& s) {
            if (frame.contains(get_pos(s))) {
                over = true;
            }
            else {
                over = false;
            }
        },
    }, state);
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
                //state = Consumed{};
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
struct Plugin_state {
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
        auto actions() const -> std::span<const User_action>
        {
            if (_actions) {
                return std::span{*_actions};
            }
            return {};
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
using Actions = Action_queue::Receiver;

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
        auto execute_all() const -> void
        {
            if (_queue) {
                auto task = Task{};
                while (_queue->pop(task)) task();
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
using Tasks = Task_queue::Receiver;

// MARK: - undo history

struct Undo_history {

    struct Param_change {
        uint32_t addr{};
        double from{};
        double to{};
    };

    struct Undo_step {
        std::vector<Param_change> changes{};
    };

    struct Receiver {
        explicit Receiver(Undo_history* history = nullptr) : _history{history} {}
        auto undo() const -> void
        {
            if (_history) { _history->defer_undo(); }
        }
        auto redo() const -> void
        {
            if (_history) { _history->defer_redo(); }
        }
    private:
        Undo_history* _history{nullptr};
    };

    Undo_history(const std::vector<Param_spec>& specs) : _specs{specs} {}

    auto process_actions(std::span<const User_action> actions, Processor_state& state) -> void
    {
        _process_actions(actions, state);
    }

    auto undo(Actions actions) -> void
    {
        _apply<true>(actions);
    }

    auto redo(Actions actions) -> void
    {
        _apply<false>(actions);
    }

    auto make_receiver() -> Receiver
    {
        return Receiver{this};
    }

    auto defer_undo() -> void
    {
        _deferred = Deferred_action{Undo{}};
    }

    auto defer_redo() -> void
    {
        _deferred = Deferred_action{Redo{}};
    }

    auto process_deferred(Actions actions) -> void
    {
        if (_deferred) {
            std::visit(Inline_visitor{
                [&](const Undo&) {
                    _apply<true>(actions);
                },
                [&](const Redo&) {
                    _apply<false>(actions);
                }
            }, *_deferred);
            _deferred.reset();
        }
    }

private:

    std::vector<Param_spec> _specs{};

    struct Undo {}; struct Redo {};
    using Deferred_action = std::variant<Undo, Redo>;

    std::optional<Deferred_action> _deferred{};

    using Active_map = std::unordered_map<uint32_t, Param_change>;

    std::optional<Active_map> _current{};
    size_t _active{};

    std::vector<Undo_step> _undo_stack{};
    std::vector<Undo_step> _redo_stack{};

    auto undoable(uint32_t addr) const -> bool
    {
        const auto& spec = _specs[addr];
        return spec.policy != Host_policy::interface;
    }

    auto _process_actions(std::span<const User_action> actions, Processor_state& state) -> void
    {
        const auto& params = state.params;

        for (const auto& action : actions) {
            std::visit(Inline_visitor{
                [&](const Action_start& s) {
                    if (!undoable(s.address)) return;
                    ++_active;
                    if (_active == 1) {
                        _current = Active_map{};
                    }
                    if (_current) {
                        _current->emplace(s.address, Param_change{
                            .addr = s.address,
                            .from = params[s.address],
                            .to = params[s.address],
                        });
                    }
                },
                [&](const Set_param& p) {
                    if (!undoable(p.address)) return;
                    if (!_current) return;
                    auto it = _current->find(p.address);
                    if (it != _current->end()) {
                        it->second.to = p.value;
                    }
                },
                [&](const Action_end& e) {
                    if (!undoable(e.address)) return;
                    if (_active == 0) return;
                    --_active;
                    if (_active == 0 && _current) {
                        Undo_step step{};
                        for (const auto& [_, change] : *_current) {
                            if (change.from != change.to) {
                                step.changes.push_back(change);
                            }
                        }
                        if (!step.changes.empty()) {
                            _undo_stack.push_back(std::move(step));
                            _redo_stack.clear();
                        }
                        _current.reset();
                    }
                },
                [&](const auto&) {}
            }, action);
        }
    }

    template<bool is_undo>
    auto _apply(Actions actions) -> void
    {
        auto& stack_from = is_undo ? _undo_stack : _redo_stack;
        auto& stack_to = is_undo ? _redo_stack : _undo_stack;

        if (stack_from.empty()) return;

        const auto step = stack_from.back();
        stack_from.pop_back();

        for (const auto& change : step.changes) {
            actions.push(Action_start{change.addr});
            actions.push(Set_param{change.addr, is_undo ? change.from : change.to});
            actions.push(Action_end{change.addr});
        }

        stack_to.push_back(step);
    }
};
using Undo_redo = Undo_history::Receiver;

struct Edit_context {
    Actions actions{};
    Tasks tasks{};
    Undo_redo undo_redo{};
};

template<typename... Args>
struct Later {

    std::function<void(Args...)> callback{[](Args...) {}};
    std::optional<Tasks> receiver{};

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

template<typename M, typename S, typename A0, typename A1, typename C, typename V, typename A, typename T, typename U>
constexpr auto run_frame(
    const M& _meter_infos,
    const S& _receiver,
    A0& _ui_params, 
    A1& _ui_meters, 
    const C& view_context, 
    V* _custom_view,
    A& _actions,
    T& _tasks,
    U& _undo_history
) -> void
{
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
                const auto type = _meter_infos.policy_for(e.address);

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
    auto state = Plugin_state{
        .processor_state = {_ui_params, meter_arr},
        .view_context = view_context,
    };
    _actions.clear(); // Actually clear before we draw.

    // Tell the user view to draw.
    _custom_view->on_gui_draw(state);

    _tasks.execute_all(); // Execute laters (might push into actions).

    // Observe actions for undo/redo.
    _undo_history.process_actions(_actions.actions(), state.processor_state);

    // Process deferred undo/redo actions (pushes into actions).
    _undo_history.process_deferred(_actions.make_receiver());

    // Handle actions and update local state.
    for (auto& action : _actions.actions()) {
        _receiver.action_handler(action);
        if (const auto* s = std::get_if<Set_param>(&action)) {
            _ui_params[s->address] = s->value; // Update the local copy.
        }
    }

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