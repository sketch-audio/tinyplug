#pragma once

#include "tinyplug/tinyplug.h"
#include "models/param_model.h"

namespace tiny {

class Plug_editor {
public:

    static auto preferred_size() -> Rect_size { return {800, 600}; }

    Plug_editor(Task_manager::Actor tasks) : _tasks{tasks} {}
    ~Plug_editor() = default;

    auto on_gui_create() -> void;
    auto on_gui_show(const Edit_context&) -> void;
    auto on_gui_notify(const Ui_notification&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_params = Param_infos<Param_model>;
    using Param_address = Param_model::Param_address;

    Task_manager::Actor _tasks{};
    Frame _frame{};
    double _value{};
    std::unique_ptr<Gesture_recognizer> _click{};

    Edit_context _edit{};
    bool _dark{};

};

} // namespace tiny