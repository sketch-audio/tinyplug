#pragma once

#include "tinyplug/tinyplug.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

class Editor {
public:

    static auto preferred_size() -> Rect_size { return {800, 600}; }

    Editor(const Edit_context& edit) : _tasks{edit.tasks}, _edit{edit} { _make_gestures(); }
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void;
    auto on_gui_show() -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

    auto notify(const Host_event&) -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    // Editor-lifetime, like `_frame` below. Scoping these to a window would
    // outlive-mismatch the frame cache in on_gui_draw: the recognizers would be
    // rebuilt empty on every show while `_frame` stayed put, so the "only on
    // change" guard would never push a frame down and every click would miss.
    auto _make_gestures() -> void;

    using User_params = params::Infos<models::Params>;
    using Address = models::Params::Address;

    Task_manager::Actor _tasks{};
    // Refreshed every time a window opens and cleared when it closes — the
    // Editor outlives any one window, so this cannot be captured once.
    Window_token _window{};
    Frame _frame{};
    double _value{};
    std::unique_ptr<Gesture_recognizer> _click{};
    std::unique_ptr<Gesture_recognizer> _chain{};

    Edit_context _edit{};
    bool _dark{};

};

} // namespace tiny::plugin