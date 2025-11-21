#pragma once

#include <chrono>
#include <concepts>
#include <functional>
#include <iostream>
#include <span>
#include <variant>

#include "../platform/platform.h"

#include "tiny_edit.h"
#include "tiny_events.h"
#include "tiny_meters.h"
#include "tiny_params.h"
#include "tiny_queue.h"
#include "tiny_utils.h"

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
inline auto try_consume(Pointer_state& state, const Frame& frame, bool actually_consume = false) -> std::optional<Pointer_state>
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
                if (actually_consume) { // Until we build new gesture system, scroll views for example actually want to consume here.
                    state = Consumed{};
                }
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
        for (const auto& e : es | std::views::reverse) {
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

class Gesture_recognizer {
public:
    virtual auto set_frame(const Frame& frame) -> void = 0;
    virtual auto process_events(Event_list& events) -> void = 0;
    virtual ~Gesture_recognizer() = default;
};

template<typename T>
using On_started = std::function<void(const T&)>;

template<typename T>
using On_updated = std::function<void(const T&)>;

template<typename T>
using On_ended = std::function<void(const T&)>;

using On_cancelled = std::function<void()>;

template<typename T>
struct Gesture_callbacks {
    On_started<T> on_started{[](auto) {}};
    On_updated<T> on_updated{[](auto) {}};
    On_ended<T> on_ended{[](auto) {}};
    On_cancelled on_cancelled{[]() {}};
};

// MARK: - Over

class Over_recognizer : public Gesture_recognizer {
public:
    
    struct Info {
        Coords pos{};
        bool over{};
    };

    explicit Over_recognizer(const Gesture_callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override
    {
        _frame = frame;

        if (_over) {
            _callbacks.on_cancelled();
            _over = false;
        }
    }

    auto process_events(Event_list& events) -> void override
    {
        for (const auto& event : events.events) {
            if (event.consumed) continue;
            std::visit(Inline_visitor{
                [&](const Pointer_move& move) {
                    const auto was_over = _over;
                    const auto now_over = _frame.contains(move.pos);
                    _resolve_events(move.pos, was_over, now_over);
                },
                [&](const Pointer_enter& enter) {
                    const auto was_over = _over;
                    const auto now_over = _frame.contains(enter.pos);
                    _resolve_events(enter.pos, was_over, now_over);
                },
                [&](const Pointer_exit& exit) {
                    _resolve_events(exit.pos, _over, false);
                },
                [](const auto&) {}
            }, event.event);
        }
    }

private:
    
    Gesture_callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _over{};

    auto _resolve_events(Coords pos, bool was_over, bool now_over) -> void
    {
        if (!was_over && now_over) {
            _callbacks.on_started({pos, true});
        }
        else if (was_over && !now_over) {
            _callbacks.on_ended({pos, false});
        }
        _over = now_over;
    }
};

using Over_info = Over_recognizer::Info;

// MARK: - Down

class Down_recognizer : public Gesture_recognizer {
public:

    struct Info {
        Coords pos{};
        bool down{};
    };

    explicit Down_recognizer(const Gesture_callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override
    {
        _frame = frame;

        if (_down) {
            _callbacks.on_cancelled();
            _down = false;
        }
    }

    auto process_events(Event_list& events) -> void override
    {
        for (const auto& event : events.events) {
            if (event.consumed) continue;
            std::visit(Inline_visitor{
                [&](const Pointer_down& down) {
                    if (_frame.contains(down.pos)) {
                        _callbacks.on_started({down.pos, true});
                        _down = true;
                    }
                },
                [&](const Pointer_up& up) {
                    if (_down) { // Event could have left our frame
                        _callbacks.on_ended({up.pos, false});
                        _down = false;
                    }
                },
                [](const auto&) {}
            }, event.event);
        }
    }

private:

    Gesture_callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _down{};

};

using Down_info = Down_recognizer::Info;

// MARK: - Dwell

class Dwell_recognizer : public Gesture_recognizer {
public:

    struct Info {
        Coords pos{};
        bool dwelling{};
    };

    explicit Dwell_recognizer(const Gesture_callbacks<Info>& callbacks)
        : _callbacks{callbacks} {}

    auto set_frame(const Frame& frame) -> void override
    {
        _frame = frame;

        if (_dwelling) {
            _callbacks.on_cancelled();
            _dwelling = false;
        }
    }

    auto process_events(Event_list& events) -> void override
    {
        for (const auto& event : events.events) {
            if (event.consumed) continue;
            std::visit(Inline_visitor{
                [&](const Pointer_down& down) {
                    if (_dwelling) {
                        _end_dwell(down.pos);
                        _over_t = std::nullopt;
                    }
                    _down = true; // Global down tracking.
                },
                [&](const Pointer_up&) {
                    _down = false;
                },
                [&](const Pointer_move& move) {
                    if (_down) {
                        _over_t = std::nullopt;
                        return;
                    }

                    if (_dwelling) {
                        _end_dwell(move.pos);
                        _over_t = std::nullopt;
                        return;
                    }

                    if (!_frame.contains(move.pos)) {
                        _over_t = std::nullopt;
                        return;
                    }

                    _over_t = events.timestamp;
                    _pos = move.pos;
                },
                [&](const auto&) {
                    if (_dwelling) {
                        _end_dwell(_pos);
                    }
                }
            }, event.event);
        }

        if (_over_t) {
            const auto now = Steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *_over_t);
            if (!_dwelling && elapsed.count() > dwell_ms) {
                _callbacks.on_started({_pos, true});
                _dwelling = true;
            }
        }
        else {
            if (_dwelling) {
                _end_dwell(_pos);
            }
        }
        
    }

private:

    static constexpr auto dwell_ms = 2000;

    Gesture_callbacks<Info> _callbacks{};
    Frame _frame{};
    bool _down{}; // No dwell when down.

    std::optional<Steady_time> _over_t{};
    Coords _pos{};
    bool _dwelling{};

    auto _end_dwell(Coords pos) -> void
    {
        _callbacks.on_ended({pos, false});
        _dwelling = false;
    }
};

using Dwell_info = Dwell_recognizer::Info;

// MARK: - Click

class Click_recognizer : public Gesture_recognizer {
public:

    struct Desc {
        Pointer_button button{};
        uint32_t count{1};
    };

    struct Info {
        Coords pos{};
    };

    explicit Click_recognizer(Gesture_callbacks<Info> callbacks, const Desc& desc) : _callbacks{callbacks}, _desc{desc} {}

    auto set_frame(const Frame& frame) -> void override
    {
        _frame = frame;
    }

    auto process_events(Event_list& events) -> void override
    {
        for (const auto& event : events.events) {
            if (event.consumed) continue;
            std::visit(Inline_visitor{
#if PLATFORM_APPLE
                [&](const Pointer_down& down) {
                    // Right click executes on pointer down on macOS.
                    if (_frame.contains(down.pos) && down.button == Pointer_button::right) {
                        const auto match = (_desc.button == down.button && _desc.count == 1);
                        if (match) _callbacks.on_started({down.pos});
                    }
                },
#endif
                [&](const Pointer_click& click) {
                    if (_frame.contains(click.pos)) {
                        const auto match = (_desc.button == click.button && _desc.count == click.count);
                        if (match) _callbacks.on_started({click.pos});
                    }
                },
                [](const auto&) {}
            }, event.event);
        }
    }

private:

    Gesture_callbacks<Info> _callbacks{};
    Desc _desc{};
    Frame _frame{};

};

using Click_info = Click_recognizer::Info;

// MARK: - Drag

class Drag_recognizer : public Gesture_recognizer {
public:

    struct Info {
        Coords fpos{};
        Coords tpos{};
    };

    explicit Drag_recognizer(const Gesture_callbacks<Info>& callbacks, bool greedy = false)
        : _callbacks{callbacks}, _greedy{greedy} {}

    auto set_frame(const Frame& frame) -> void override
    {
        _frame = frame;

        if (_fpos.has_value() || _tag.has_value() || _initiated) {
            _callbacks.on_cancelled();
            _reset();
        }
    }

    auto process_events(Event_list& events) -> void override
    {
        for (auto& event : events.events) {
            if (event.consumed) continue;
            if (_tag && *_tag != event.pointer_tag) continue; // Skip events for unbound pointers

            std::visit(Inline_visitor{
                [&](const Pointer_down& down) {
                    if (down.button != Pointer_button::left) return; // Only primary button drags.
                    if (_frame.contains(down.pos)) {
                        // set
                        _fpos = down.pos;
                        _tag = event.pointer_tag;
                        _initiated = false;

                        event.consumed = _greedy ? true : event.consumed;
                    }
                },
                [&](const Pointer_up& up) {
                    if (_fpos && _initiated) {
                        _callbacks.on_ended({*_fpos, up.pos});
                        event.consumed = _greedy ? true : event.consumed;
                    }
                    _reset();
                },
                [&](const Pointer_move& move) {
                    if (_fpos) {
                        if (!_initiated) {
                            _callbacks.on_started({*_fpos, move.pos});
                            _initiated = true;
                        }
                        else {
                            _callbacks.on_updated({*_fpos, move.pos});
                        }
                        event.consumed = _greedy ? true : event.consumed;
                    }
                },
                [](const auto&) {}
            }, event.event);
        }
    }

private:

    Gesture_callbacks<Info> _callbacks{};
    bool _greedy{};

    Frame _frame{};

    std::optional<Coords> _fpos{};
    std::optional<uintptr_t> _tag{};
    bool _initiated{};

    auto _reset() -> void
    {
        _fpos = std::nullopt;
        _tag = std::nullopt;
        _initiated = false;
    }
};

using Drag_info = Drag_recognizer::Info;

// Factory to create recognizers with deduced callback types.
template<typename Recognizer, typename... Args>
auto make_recognizer(Gesture_callbacks<typename Recognizer::Info> callbacks, Args&&... args) -> std::unique_ptr<Recognizer>
{
    return std::make_unique<Recognizer>(std::move(callbacks), std::forward<Args>(args)...);
}

// MARK: - user interaction

// A user interaction includes an id (for future multi-touch), pointer state, and scroll deltas.
struct User_interaction {
    std::vector<Pointer> pointers{};
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

    _actions.process_observers(_ui_params); // Use manifested state.

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
