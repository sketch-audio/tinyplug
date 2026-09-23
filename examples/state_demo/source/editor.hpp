#pragma once

#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Paint the gate pattern (state) and drag Depth (a parameter). Both land in one undo
// history: a stroke is one step, a Depth drag is one step, and Undo/Redo walk them in order.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {640, 300}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto notify(const Host_event&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using Address = models::Params::Address;
    using Meter = models::Meters::Address;

    struct Layout {
        Frame grid{};
        Frame undo{};
        Frame redo{};
        Frame depth{};
    };

    Edit_context _edit{};
    bool _dark{true};

    bool _painting{};
    int _last_column{};
    int _last_level{};

    bool _dragging_depth{};
    double _depth{};

    auto _layout(Rect_size size) const -> Layout;
    auto _paint(const Frame& grid, Coords pos, bool first) -> void;
    auto _end_stroke() -> void;
    auto _set_depth(const Frame& strip, Coords pos) -> void;

};

} // namespace tiny::edit
