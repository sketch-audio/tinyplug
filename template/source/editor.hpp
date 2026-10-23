#pragma once

#include <tinyplug/tinyplug.hpp>
#include "models/params.hpp"

namespace tiny::edit {

class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {200, 400}}; }

    Editor(const edit::Context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void;
    auto on_gui_show() -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

private:

    using Address = models::Params::Address;

    Frame _frame{};
    Coords _translation{};
    double _value{};
    std::unique_ptr<Gesture_recognizer> _drag{};

    edit::Context _edit{};
    bool _dark{};

};
static_assert(Interface<Editor>);

} // namespace tiny::edit