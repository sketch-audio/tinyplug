#pragma once

#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// The mode buttons and Click, then the handshake: what the parameter wants, the proposal the
// host hasn't accepted yet (red, with how long it has waited), what the processor renders with,
// and the last steps.
class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {520, 400}}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void { _controls.cancel(_edit.actions); }
    auto on_gui_destroy() -> void {}

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;

    Edit_context _edit{};
    bool _dark{true};
    ui::Controls _controls{};

};

} // namespace tiny::edit
