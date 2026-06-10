#pragma once

#include "tinyplug/tinyplug.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

class Editor {
public:

    static auto preferred_size() -> Rect_size { return {200, 400}; }

    Editor(Task_manager::Actor) {}
    ~Editor() = default;

    auto on_gui_create() -> void;
    auto on_gui_show(const Edit_context&) -> void;
    auto on_gui_notify(const Ui_notification&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_params = Param_infos<models::Params>;
    using Address = models::Params::Address;

    Frame _frame{};
    Coords _translation{};
    double _value{};
    std::unique_ptr<Gesture_recognizer> _drag{};

    Edit_context _edit{};
    bool _dark{};

};

} // namespace tiny::plugin