#pragma once

#include <array>

#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Input meters, the gain fader, output meters. Holds latch the highest peak, read out in dBFS
// under each bar, until clicked.
class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {320, 360}}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void { _controls.cancel(_edit.actions); }
    auto on_gui_destroy() -> void {}

private:

    using Address = models::Params::Address;

    Edit_context _edit{};
    bool _dark{true};
    ui::Controls _controls{};
    std::array<double, User_meters::num_meters> _holds{};

};

} // namespace tiny::edit
