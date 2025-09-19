#pragma once

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

class Plug_editor {
public:

    static auto preferred_size() -> Rect_size { return {400, 400}; }

    Plug_editor() = default;
    ~Plug_editor() = default;

    auto on_gui_create() -> void {}
    auto on_gui_show(const View_connection&) -> void;
    auto on_gui_notify(const Ui_notification&) -> void;
    auto on_gui_draw(App_state&) -> void;
    auto on_gui_hide() -> void {}
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_params = Param_infos<Param_model>;
    using Param_id = Param_model::Param_id;
    using Export_id = Param_model::Export_id;

    User_params _params{};
    Action_queue::Receiver _actions{};
    Task_queue::Receiver _task_receiver{};

};

} // namespace tiny