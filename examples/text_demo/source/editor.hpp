#pragma once

#include <tiny_text/fonts.hpp>
#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Text with tiny_text and Inter, a variable font. Single line: a specimen at the Size and Weight parameters,
// a weight ladder, kerning, proportional against tabular figures, italic, and the font's metrics at the
// specimen. Placement: each anchor on a guide line, tracked capitals centred on their letters, case-sensitive
// forms, optical alignment, overflow. Multiline and spans: a paragraph wrapped and held to three lines,
// value-and-unit readouts, mixed styles in one paragraph. Sweep animates the weight axis live; the header
// reads frame rate and what the caches hold.
class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {1040, 720}, .min = {860, 640}, .max = Rect_size{1800, 1200}}; }

    explicit Editor(const Edit_context& edit);
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void { _controls.cancel(_edit.actions); }
    auto on_gui_destroy() -> void {}

private:

    using Address = models::Params::Address;

    Edit_context _edit{};
    ui::Controls _controls{};
    text::Fonts _fonts;

    Time_point _start{};
    Time_point _last{};
    double _fps{};

};

} // namespace tiny::edit
