#pragma once

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"

namespace tiny::plugin {

class Editor {
public:

    static auto preferred_size() -> Rect_size { return {300, 300}; }

    Editor(const Edit_context&) {}
    ~Editor() = default;

    auto on_gui_create() -> void {}
    auto on_gui_show() -> void {}
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void {}
    auto on_gui_destroy() -> void {}

    auto notify(const Host_event&) -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using Meter = models::Meters::Address;
};

} // namespace tiny::plugin
