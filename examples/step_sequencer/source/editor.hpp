#pragma once

#include <array>

#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// A one-octave piano roll of sixteen steps. Click a cell to put that note on the step, click it
// again for a rest; drag across steps to draw a line. Gate and Velocity are the strips on the
// right. The pattern is the state document, so it saves with the session and undoes.
class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {640, 320}}; }

    Editor(const edit::Context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_create(Gui_info) -> void {}
    auto on_show() -> void {}
    auto on_draw(Plugin_state&) -> void;
    auto on_hide() -> void;
    auto on_destroy() -> void {}

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

    edit::Context _edit{};
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
static_assert(Interface<Editor>);

} // namespace tiny::edit
