#include "editor.hpp"

#include <embedded/fonts.hpp>
#include <tiny_platform/platform_text.hpp>
#include <tiny_ui/draw.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <numbers>

namespace tiny::edit {

namespace {

auto faces() -> std::array<text::Face, 2>
{
    return {{
        {.family = "Inter", .data = embedded::fonts::intervariable_ttf()},
        {.family = "Inter", .data = embedded::fonts::intervariable_italic_ttf()},
    }};
}

constexpr auto prose =
    "A variable font carries its whole design space in one file: weight, optical size, and whatever "
    "else its designer exposed. HarfBuzz shapes each run against the instance it will be drawn with, "
    "so kerning and advances follow the axes as they move. Resize the window to rewrap this paragraph.";
constexpr auto long_name = "Warm Plate, Long Tail Shimmer";

// std::format's floating point needs macOS 13.3; the demos target 11.
template<typename... Args>
auto print(const char* format, Args... args) -> std::string
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    return buffer;
}

auto plain(std::span<const double> knobs, models::Params::Address address) -> double
{
    const auto a = enum_raw(address);
    return params::Value_helper::knob_to_plain(knobs[a], User_params::param_spec(a).semantics);
}

auto inter(float size, float weight = 400) -> text::Style { return {.family = "Inter", .size = size, .weight = weight}; }

auto tabular(text::Style s) -> text::Style
{
    s.features[0] = {text::tag("tnum")};
    return s;
}

} // namespace

Editor::Editor(const Edit_context& edit) : _edit{edit}, _fonts{faces()}, _start{System_clock::now()} {}

auto Editor::on_gui_hide() -> void
{
    _controls.cancel(_edit.actions);
    Platform_text::cancel(_window);
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view = state.view_context;
    auto* canvas = view.canvas;
    if (!canvas) return;

    const auto t = ui::theme(view.dark);
    const auto& knobs = state.processor_state.params;
    const auto ink = t.text;
    const auto dim = t.dim;
    const auto guide = ui::red;
    const auto accent = ui::blue;

    const auto seconds = Durations::delta_secs(_start, view.time_now);
    if (_last != Time_point{}) {
        const auto dt = Durations::delta_secs(_last, view.time_now);
        if (dt > 0) _fps = _fps == 0 ? 1 / dt : 0.95 * _fps + 0.05 * (1 / dt);
    }
    _last = view.time_now;

    const auto sweep = knobs[enum_raw(Address::Sweep)] >= 0.5;
    const auto weight = static_cast<float>(sweep ? 500 + 400 * std::sin(seconds * std::numbers::pi * 0.5) : plain(knobs, Address::Weight));
    const auto size = static_cast<float>(plain(knobs, Address::Size));

    const auto w = static_cast<double>(view.logical_size.w);
    const auto h = static_cast<double>(view.logical_size.h);
    const auto page = ui::inset({0, 0, w, h}, 24);

    // Controls: tiny_ui's tracks and fills, labelled with tiny_text.
    const auto bar = Frame{page.x, page.y + 44, page.w, 24};
    const auto controls = std::array<ui::Control, 3>{{
        {ui::column(bar, 0, 3, 12), enum_raw(Address::Weight), ui::Control::Kind::Slider, "", ui::blue},
        {ui::column(bar, 1, 3, 12), enum_raw(Address::Size), ui::Control::Kind::Slider, "", ui::purple},
        {ui::column(bar, 2, 3, 12), enum_raw(Address::Sweep), ui::Control::Kind::Toggle, "", ui::green},
    }};
    const auto body = Frame{page.x, bar.y + bar.h + 24, page.w, page.y + page.h - (bar.y + bar.h + 24)};
    const auto col = [&](int i) { return ui::column(body, i, 3, 28); };
    const auto specimen = Frame{col(0).x, col(0).y + 22, col(0).w, std::max(size * 1.3, 24.)};

    // Typing: a right-click (or Ctrl-click) on a slider types its value; a click on the specimen types new text.
    for (auto& event : view.interaction.events.events) {
        if (const auto* down = std::get_if<Pointer_down>(&event.event); down && down->button == Pointer_button::right) {
            for (auto i = 0; i < 2; ++i) {
                if (!controls[static_cast<size_t>(i)].frame.contains(down->pos)) continue;
                event.consumed = true;
                _type_value(i == 0 ? Address::Weight : Address::Size, controls[static_cast<size_t>(i)].frame, knobs, view.dark);
            }
        }
        if (const auto* click = std::get_if<Pointer_click>(&event.event); click && click->button == Pointer_button::left && specimen.contains(click->pos)) {
            event.consumed = true;
            _type_specimen(specimen, size, weight, view.dark);
        }
    }
    _controls.interact(view.interaction, _edit.actions, controls, knobs);

    canvas->save();
    canvas->scale(static_cast<float>(view.scale), static_cast<float>(view.scale));
    ui::fill(*canvas, {0, 0, w, h}, t.background);

    const auto hline = [&](double y, double x0, double x1) { ui::fill(*canvas, {x0, y - 0.5, x1 - x0, 1}, guide); };
    const auto vline = [&](double x, double y0, double y1) { ui::fill(*canvas, {x - 0.5, y0, 1, y1 - y0}, guide); };

    const auto label = tabular(inter(12, 500));
    const auto small = tabular(inter(11));
    const auto note = [&](std::string_view s, double x, double y, text::Align align = text::Align::Left) {
        _fonts.text(*canvas, s, small, {x, y}, dim, {.align = align});
    };

    // Header: the title at the live weight, and what drawing costs.
    _fonts.text(*canvas, "Text Demo", inter(28, weight), {page.x, page.y + 17}, ink);
    _fonts.text(*canvas, print("%.0f fps  ·  %zu instances  ·  %zu layouts", _fps, _fonts.cached_instances(), _fonts.cached_layouts()),
                small, {page.x + page.w, page.y + 17}, dim, {.align = text::Align::Right});

    ui::draw_controls(*canvas, t, controls, knobs);
    const auto value_label = [&](int i, const std::string& s) {
        _fonts.line(*canvas, s, label, ui::inset(controls[static_cast<size_t>(i)].frame, 8), ink);
    };
    value_label(0, print("Weight  %.0f", static_cast<double>(weight)));
    value_label(1, print("Size  %.1f", static_cast<double>(size)));
    value_label(2, sweep ? "Sweep  on" : "Sweep  off");

    const auto heading = text::Style{.family = "Inter", .size = 11, .weight = 650, .tracking = 80, .transform = text::Case::Upper};

    // MARK: single line

    const auto c1 = col(0);
    auto y = c1.y;
    _fonts.text(*canvas, "Single line", heading, {c1.x, y + 6}, dim);
    y += 22;

    ui::outline(*canvas, specimen, t.track);
    _fonts.line(*canvas, _specimen, inter(size, weight), specimen, ink);
    y += specimen.h + 14;

    for (auto wt = 100; wt <= 900; wt += 200) {
        const auto row_y = y + 10;
        note(print("%d", wt), c1.x + 26, row_y, text::Align::Right);
        _fonts.text(*canvas, "Hamburgefonstiv 0123", inter(15, static_cast<float>(wt)), {c1.x + 36, row_y}, ink);
        y += 22;
    }
    y += 8;

    _fonts.text(*canvas, "AVATAR Wave To Ty LT", inter(22, 500), {c1.x, y + 12}, ink);
    y += 36;

    const auto counter = print("%7.2f s", seconds); // Proportional figures shift as they change; tabular don't.
    note("proportional", c1.x, y + 8);
    _fonts.text(*canvas, counter, inter(16, 500), {c1.x + c1.w, y + 8}, ink, {.align = text::Align::Right});
    y += 22;
    note("tabular", c1.x, y + 8);
    _fonts.text(*canvas, counter, tabular(inter(16, 500)), {c1.x + c1.w, y + 8}, ink, {.align = text::Align::Right});
    y += 30;

    _fonts.text(*canvas, "Italic, at the live weight", text::Style{.family = "Inter", .size = 16, .weight = weight, .italic = true}, {c1.x, y + 8}, ink);
    y += 34;

    // The font at the specimen's style: table heights against outline heights.
    const auto fm = _fonts.metrics(inter(size, weight));
    _fonts.text(*canvas, "Metrics at the specimen", heading, {c1.x, y + 6}, dim);
    y += 20;
    const auto metric_rows = std::array{
        std::pair{"cap height", print("%.2f  ink %.2f", fm.cap_height, fm.ink_cap_height)},
        std::pair{"x height", print("%.2f  ink %.2f", fm.x_height, fm.ink_x_height)},
        std::pair{"ascent / descent", print("%.2f / %.2f", fm.ascent, fm.descent)},
        std::pair{"line height", print("%.2f", fm.line_height)},
    };
    for (const auto& [name, value] : metric_rows) {
        note(name, c1.x, y + 7);
        _fonts.text(*canvas, value, small, {c1.x + c1.w, y + 7}, ink, {.align = text::Align::Right});
        y += 17;
    }

    // MARK: placement

    const auto c2 = col(1);
    auto py = c2.y;
    _fonts.text(*canvas, "Placement", heading, {c2.x, py + 6}, dim);
    py += 22;

    // Each anchor on its own guide line.
    const auto anchors = std::array{
        std::pair{text::Anchor::Cap, "Cap"}, std::pair{text::Anchor::X, "X"}, std::pair{text::Anchor::Ink, "Ink"},
        std::pair{text::Anchor::Box, "Box"}, std::pair{text::Anchor::Baseline, "Baseline"},
    };
    for (const auto& [anchor, name] : anchors) {
        const auto line_y = py + 24;
        const auto sample = inter(24, 500);
        const auto x0 = c2.x + 62;
        // What the anchor centres, from the public metrics: the guide bisects it.
        const auto fm = _fonts.metrics(sample);
        const auto bounds = _fonts.ink("Hxgy (12)", sample);
        const auto width = _fonts.measure("Hxgy (12)", sample).width;
        const auto band = [&](double top, double bottom, double left, double right) {
            ui::fill(*canvas, {left, top, right - left, bottom - top}, SkColorSetA(accent, 40));
        };
        switch (anchor) {
            case text::Anchor::Cap: band(line_y - fm.ink_cap_height / 2, line_y + fm.ink_cap_height / 2, x0, x0 + width); break;
            case text::Anchor::X: band(line_y - fm.ink_x_height / 2, line_y + fm.ink_x_height / 2, x0, x0 + width); break;
            case text::Anchor::Ink: {
                const auto baseline = line_y - (bounds.top + bounds.bottom) / 2;
                band(baseline + bounds.top, baseline + bounds.bottom, x0 + bounds.left, x0 + bounds.right);
                break;
            }
            case text::Anchor::Box: {
                const auto baseline = line_y + (fm.ascent - fm.descent) / 2;
                band(baseline - fm.ascent, baseline + fm.descent, x0, x0 + width);
                break;
            }
            case text::Anchor::Baseline: break;
        }
        hline(line_y, c2.x + 58, c2.x + c2.w);
        note(name, c2.x + 50, line_y, text::Align::Right);
        _fonts.text(*canvas, "Hxgy (12)", sample, {x0, line_y}, ink, {.anchor = anchor});
        py += 44;
    }
    py += 2;

    // Tracked capitals centred on their letters, and case-sensitive forms.
    const auto cx = c2.x + c2.w / 2;
    vline(cx, py, py + 20);
    _fonts.text(*canvas, "output gain", text::Style{.family = "Inter", .size = 13, .weight = 600, .tracking = 160, .transform = text::Case::Upper},
                {cx, py + 10}, ink, {.align = text::Align::Center});
    py += 30;
    note("Case::Upper", c2.x, py + 11);
    _fonts.text(*canvas, "(x-y) @ 12:30", text::Style{.family = "Inter", .size = 24, .weight = 500, .transform = text::Case::Upper},
                {c2.x + c2.w, py + 11}, ink, {.align = text::Align::Right});
    py += 30;
    note("typed capitals", c2.x, py + 11);
    _fonts.text(*canvas, "(X-Y) @ 12:30", inter(24, 500), {c2.x + c2.w, py + 11}, ink, {.align = text::Align::Right});
    py += 34;

    // Optical alignment: the T's side bearing against the outline's edge.
    const auto edge = c2.x + 8;
    vline(edge, py, py + 92);
    _fonts.text(*canvas, "Tempo", inter(44, 600), {edge, py + 22}, ink);
    note("advance", c2.x + c2.w, py + 22, text::Align::Right);
    py += 48;
    _fonts.text(*canvas, "Tempo", inter(44, 600), {edge, py + 22}, ink, {.optical = true});
    note("optical", c2.x + c2.w, py + 22, text::Align::Right);
    py += 54;

    // Overflow in a narrow frame.
    const auto overflows = std::array{
        std::pair{text::Overflow::Ellipsis, "Ellipsis"}, std::pair{text::Overflow::Shrink, "Shrink"}, std::pair{text::Overflow::Clip, "Clip"},
    };
    for (const auto& [overflow, name] : overflows) {
        const auto frame = Frame{c2.x, py, c2.w * 0.66, 26};
        ui::outline(*canvas, frame, t.track);
        const auto m = _fonts.line(*canvas, long_name, inter(15, 500), ui::inset(frame, 6), ink, {.overflow = overflow, .min_size = 10});
        note(overflow == text::Overflow::Shrink ? print("%s %.1f", name, static_cast<double>(m.size)) : std::string{name}, c2.x + c2.w, py + 13, text::Align::Right);
        py += 32;
    }

    // MARK: multiline and spans

    const auto c3 = col(2);
    auto ry = c3.y;
    _fonts.text(*canvas, "Multiline and spans", heading, {c3.x, ry + 6}, dim);
    ry += 22;

    const auto body_style = text::Style{.family = "Inter", .size = 14, .weight = sweep ? weight : 400.f, .line_height = 1.45f};
    const auto wrapped = _fonts.paragraph(*canvas, prose, body_style, {c3.x, ry, c3.w, 0}, ink);
    ui::outline(*canvas, {c3.x - 4, ry - 4, c3.w + 8, wrapped.height + 8}, t.track);
    ry += wrapped.height + 10;
    note(print("%zu lines, %.1f high", wrapped.lines, wrapped.height), c3.x, ry + 7);
    ry += 22;

    const auto held = _fonts.paragraph(*canvas, prose, body_style, {c3.x, ry, c3.w, 0}, ink, text::Align::Left, 3);
    ui::outline(*canvas, {c3.x - 4, ry - 4, c3.w + 8, held.height + 8}, t.track);
    ry += held.height + 24;

    // A readout: value and unit as spans on one baseline, the value's capitals centred on the line.
    const auto readout = [&](double line_y, const std::string& value, std::string_view unit, std::string_view name) {
        const auto spans = std::array{
            text::Span{value, tabular(inter(30, 550)), ink},
            text::Span{unit, inter(15, 450), dim},
        };
        hline(line_y, c3.x + 56, c3.x + c3.w);
        _fonts.text(*canvas, spans, {c3.x + c3.w, line_y}, {.align = text::Align::Right});
        note(name, c3.x, line_y);
    };
    readout(ry + 16, print("%.1f", static_cast<double>(size)), " pt", "size");
    ry += 44;
    readout(ry + 16, print("%.0f", static_cast<double>(weight)), " wght", "weight");
    ry += 48;

    const auto rich = std::array{
        text::Span{"Spans mix ", inter(14), ink},
        text::Span{"weights", inter(14, 700), ink},
        text::Span{", ", inter(14), ink},
        text::Span{"colours", inter(14, 500), accent},
        text::Span{" and ", inter(14), ink},
        text::Span{"styles", text::Style{.family = "Inter", .size = 14, .italic = true}, ink},
        text::Span{" in one paragraph, and wrap as one.", inter(14), ink},
    };
    _fonts.paragraph(*canvas, rich, {c3.x, ry, c3.w, 0});

    canvas->restore();
}

auto Editor::_type_value(Address address, const Frame& frame, std::span<const double> knobs, bool dark) -> void
{
    const auto a = enum_raw(address);
    const auto& spec = User_params::param_spec(a);
    const auto t = ui::theme(dark);
    const auto field = Text_field{
        .frame = frame,
        .initial = Host_formatter::to_string(params::Value_helper::knob_to_host(knobs[a], spec.semantics), spec.semantics),
        .label = spec.name,
        .font = {.data = embedded::fonts::intervariable_ttf(), .size = 13, .weight = 500},
        .ink = t.text,
        .background = t.panel,
    };
    // One gesture, so the typed value is one undo step and the host sees a touch.
    Platform_text::edit(field, [this, a](Text_result result) {
        if (!result.committed()) return;
        const auto& semantics = User_params::param_spec(a).semantics;
        const auto plain = Host_formatter::to_value(result.text, semantics); // Plain space, though to_string takes host.
        if (!plain) return;
        _edit.actions.push(Action_start{a});
        _edit.actions.push(Set_param{a, params::Value_helper::plain_to_knob(params::Value_helper::clamp(*plain, semantics), semantics)});
        _edit.actions.push(Action_end{a});
    }, Dialog_context{_edit.tasks, _window});
}

auto Editor::_type_specimen(const Frame& frame, float size, float weight, bool dark) -> void
{
    const auto t = ui::theme(dark);
    const auto field = Text_field{
        .frame = frame,
        .initial = _specimen,
        .label = "Specimen",
        .font = {.data = embedded::fonts::intervariable_ttf(), .size = size, .weight = weight},
        .ink = t.text,
        .background = t.background,
        .align = Text_field::Align::Left,
        .select_all = false,
    };
    Platform_text::edit(field, [this](Text_result result) {
        if (result.committed() && !result.text.empty()) _specimen = result.text;
    }, Dialog_context{_edit.tasks, _window});
}

} // namespace tiny::edit
