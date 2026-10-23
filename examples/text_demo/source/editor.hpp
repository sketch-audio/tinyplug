#pragma once

#include <string>

#include <tiny_text/fonts.hpp>
#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Text with tiny_text and Inter, a variable font. Single line: a specimen at the Size and Weight parameters,
// a weight ladder, kerning, proportional against tabular figures, italic, and the font's metrics at the
// specimen. Placement: each anchor on a guide line, tracked capitals centred on their letters, case-sensitive
// forms, optical alignment, overflow. Multiline and spans: a paragraph wrapped and held to three lines,
// value-and-unit readouts, mixed styles in one paragraph. Sweep animates the weight axis live; the header
// reads frame rate and what the caches hold. Typing, in native fields drawn in Inter: right-click (or Ctrl-click)
// the Weight or Size slider to type a value, click the specimen to type your own sample.
class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {1040, 720}, .min = {860, 640}, .max = Rect_size{1800, 1200}}; }

    explicit Editor(const edit::Context& edit);
    ~Editor() = default;

    auto on_create(Gui_info info) -> void { _window = info.window; } // Text fields open in this window.
    auto on_show() -> void {}
    auto on_draw(Plugin_state&) -> void;
    auto on_hide() -> void;
    auto on_destroy() -> void {}

private:

    using Address = models::Params::Address;

    edit::Context _edit{};
    Window_token _window{};
    ui::Controls _controls{};
    text::Fonts _fonts;
    std::string _specimen{"Sphinx of black quartz, judge my vow"};

    Time_point _start{};
    Time_point _last{};
    double _fps{};

    auto _type_value(Address address, const Frame& frame, std::span<const double> knobs, bool dark) -> void;
    auto _type_specimen(const Frame& frame, float size, float weight, bool dark) -> void;

};
static_assert(Interface<Editor>);

} // namespace tiny::edit
