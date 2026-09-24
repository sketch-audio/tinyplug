#pragma once

#include <string>
#include <vector>

#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Parameters on top, the realized Value over the last four seconds with a tick where each event
// landed, and the log: every set, ramp, configure and reset the processor received, in order.
// Click the log to clear it.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {640, 520}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto notify(const Host_event&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void { _controls.cancel(_edit.actions); }
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;

    struct Line {
        std::string text{};
        int64_t frame{};
        models::Log_entry::Kind kind{};
    };

    Edit_context _edit{};
    bool _dark{true};
    ui::Controls _controls{};

    std::vector<Line> _lines{};
    uint32_t _seen{};     // The last sequence taken from the processor.
    uint32_t _missed{};   // Entries that scrolled out of the frame before a draw took them.

    auto _take(const models::Log_frame& log) -> void;

};

} // namespace tiny::edit
