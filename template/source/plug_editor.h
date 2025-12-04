#pragma once

#include "tinyplug/tinyplug.h"
#include "models/meter_model.h"
#include "models/param_model.h"

namespace tiny {

class Plug_editor {
public:

    static auto preferred_size() -> Rect_size { return {200, 400}; }

    Plug_editor() = default;
    ~Plug_editor() = default;

    auto on_gui_create() -> void {}
    auto on_gui_show(const Edit_context&) -> void;
    auto on_gui_notify(const Ui_notification&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void {}
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_params = Param_infos<Param_model>;
    using Param_address = Param_model::Param_address;

    Edit_context _edit{};
    bool _dark{};

};

} // namespace tiny