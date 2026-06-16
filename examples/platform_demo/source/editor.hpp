#pragma once

#include "tinyplug/tinyplug.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

class Editor {
public:

    static auto preferred_size() -> Rect_size { return {800, 600}; }

    Editor(const Edit_context& edit) : _tasks{edit.tasks}, _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create() -> void;
    auto on_gui_show() -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

    auto notify(const Host_event&) -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_params = params::Infos<models::Params>;
    using Address = models::Params::Address;

    Task_manager::Actor _tasks{};
    Frame _frame{};
    double _value{};
    std::unique_ptr<Gesture_recognizer> _click{};

    Edit_context _edit{};
    bool _dark{};

};

} // namespace tiny::plugin