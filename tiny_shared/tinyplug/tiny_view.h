#pragma once

#include <concepts>
#include <functional>
#include <span>
#include <variant>

#include "tiny_exports.h"

class SkCanvas; // Skia canvas

namespace tiny {

// MARK: - helpers

struct Coords {
    double x{}; double y{};
    bool operator==(const Coords&) const = default;
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
using Pointer_state = std::variant<Consumed, Over, Dwell, Click, Double_click, Drag_start, Drag, Drag_end, Right_click>;

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

struct Modifier_keys {
    // The primary platform modifier: Ctrl (Windows), command (Apple).
    bool primary{};

    // The alternate modifier: Alt (Windows), option (Apple).
    bool alt{};

    // The shift key.
    bool shift{};
};

// MARK: - user interaction

// A user interaction includes an id (for future multi-touch), pointer state, and scroll deltas.
struct User_interaction {
    int64_t id{};
    Pointer_state state{};
    Coords scroll_deltas{};
    Modifier_keys modifier_keys{};
    bool operator==(const User_interaction&) const = default;
};

// MARK: - app state

// The view context consists of the user interaction(s), a Skia canvas, a logical size, and scale.
struct View_context {
    User_interaction interaction{};
    SkCanvas* canvas{nullptr};
    Rect_size logical_size{};
    double scale{1};
    bool operator==(const View_context&) const = default;
};

// Typically the plug-in format's view will have a draw callback.
// This is where it will resolve the application state and pass it to your custom view.
using Draw_callback = std::function<void(View_context&)>;

// Read-only access to some param (and export) values.
struct Params_state {
    std::span<const double> params{};
    std::span<const double> exports{};
};

// A place to send your UI element's actions.
struct Action_receiver {
    // Add an action that took place in the current frame.
    auto add_action(const User_action& action) -> void
    {
        _actions.push_back(action);
    }
    // The plug-in format's view will use this to send your actions where they need to go.
    auto actions() const -> const std::vector<User_action>&
    {
        return _actions;
    }
private:
    std::vector<User_action> _actions{};
};

// The app state gives you
// - Read-only access to the param and export values.
// - A view context with the interaction state and a canvas in which to draw.
// - A receiver for your control's actions.
struct App_state {
    Params_state params_state{};
    View_context view_context{};
    Action_receiver action_receiver{};
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

template<typename E, typename S, typename A0, typename A1, typename C, typename V>
constexpr auto run_frame(const S& _receiver, A0& _uiparams, A1& _uiexports, const C& view_context, V* _custom_view) -> void
{
    // Pop the exports.
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                _uiparams[p.id] = p.value;
            },
            [&](const Set_export& e) {
                //
                auto& uiexport = _uiexports[e.id];
                const auto type = E::type_for(e.id);

                using enum Export_type;
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

    // Adapt tagged exports to values.
    auto export_arr = std::array<double, E::num_exports>{};
    const auto value_tx = _uiexports | std::views::transform(&Tagged_export::value);
    std::ranges::copy(value_tx, export_arr.begin());

    // Create view context.
    auto app_state = App_state{
        .params_state = {
            .params = _uiparams,
            .exports = export_arr
        },
        .view_context = view_context,
        .action_receiver = Action_receiver{},
    };

    // Tell the user view to draw.
    _custom_view->on_draw(app_state);

    // Handle actions and update local state.
    auto& actions = app_state.action_receiver.actions();
    for (auto& action : actions) {
        _receiver.action_handler(action);
        std::visit(Inline_visitor{
            [&](const Set_param& s) { _uiparams[s.id] = s.value; }, // Update local copy (assume success).
            [](const auto&) {}
        }, action);
    }

    // Reset exports for next frame.
    for (auto& uiexport : _uiexports) {
        uiexport.updated = false;
        if (uiexport.trigged) {
            uiexport.value = 0;
            uiexport.trigged = false;
        }
    }
}

} // namespace view_impl

} // namespace tiny