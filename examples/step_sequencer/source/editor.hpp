#pragma once

#include <array>

#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// A one-octave piano roll of sixteen steps. Click a cell to put that note on the step, click it
// again for a rest; drag across steps to draw a line. Gate and Velocity are the strips on the
// right. The pattern is the state document, so it saves with the session and undoes.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {640, 320}; }

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
    static constexpr auto num_params = User_params::num_params;

    struct Layout {
        Frame grid{};
        Frame undo{};
        Frame redo{};
        std::array<Frame, num_params> strips{};
    };

    Edit_context _edit{};
    bool _dark{true};

    bool _drawing{};
    bool _erasing{};  // A stroke that began on a set cell clears; otherwise it sets.
    int _dragging{-1}; // Parameter strip under a drag, or -1.

    auto _layout(Rect_size size) const -> Layout;
    auto _draw(const Frame& grid, Coords pos, bool first) -> void;
    auto _end_stroke() -> void;
    auto _set(int strip, const Frame& frame, Coords pos) -> void;
    auto _end_drag() -> void;

};

} // namespace tiny::edit
