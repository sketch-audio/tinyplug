#include "editor.hpp"

#include <tiny_ui/draw.hpp>

#include <string>

namespace tiny::edit {

namespace {

auto ms(double frames, double sr) -> std::string { return ui::fixed(frames * 1000. / sr, 1) + " MS"; }

} // namespace

auto Editor::notify(const Host_event& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view = state.view_context;
    auto* canvas = view.canvas;
    if (!canvas) return;

    const auto t = ui::theme(_dark);
    const auto& knobs = state.processor_state.params;
    const auto& hs = state.processor_state.blocks.latest<Block::Handshake>();

    const auto bounds = ui::inset({0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, 16);
    const auto buttons = Frame{bounds.x, bounds.y, bounds.w, 30};
    const auto boxes = Frame{bounds.x, buttons.y + buttons.h + 16, bounds.w, 70};
    const auto steps = Frame{bounds.x, boxes.y + boxes.h + 16, bounds.w, bounds.y + bounds.h - (boxes.y + boxes.h + 16)};

    using Kind = ui::Control::Kind;
    const auto mode = enum_raw(Address::Mode);
    const auto controls = std::array<ui::Control, 4>{{
        {ui::column(buttons, 0, 4, 8), mode, Kind::Choice, "0 MS", ui::green, 0.},
        {ui::column(buttons, 1, 4, 8), mode, Kind::Choice, "2 MS", ui::green, 0.5},
        {ui::column(buttons, 2, 4, 8), mode, Kind::Choice, "20 MS", ui::green, 1.},
        {ui::column(buttons, 3, 4, 8), enum_raw(Address::Click), Kind::Toggle, "CLICK", ui::amber},
    }};
    _controls.interact(view.interaction, _edit.actions, controls, knobs);

    canvas->save();
    canvas->scale(static_cast<float>(view.scale), static_cast<float>(view.scale));
    ui::fill(*canvas, {0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, t.background);
    ui::draw_controls(*canvas, t, controls, knobs);

    // Wanted -> proposed -> rendering. Settled is all green; in flight, the middle box is red.
    const auto box = [&](int i, const char* title, const std::string& value, SkColor color) {
        const auto f = ui::column(boxes, i, 3, 12);
        ui::fill(*canvas, f, color);
        ui::text(*canvas, f.x + 8, f.y + 8, title, t.background);
        ui::text(*canvas, f.x + 8, f.y + 30, value, t.background, 3);
    };
    const auto sr = hs.sr > 0 ? hs.sr : 48000.;
    const auto settled = hs.current == hs.wanted && hs.pending < 0;
    box(0, "WANTED", ms(hs.wanted, sr), ui::blue);
    box(1, "PROPOSED", hs.pending < 0 ? "NONE" : ms(static_cast<double>(hs.pending), sr), hs.pending < 0 ? t.track : ui::red);
    box(2, "RENDERING", ms(hs.current, sr), settled ? ui::green : ui::amber);
    if (hs.pending >= 0) {
        ui::text(*canvas, boxes.x + boxes.w / 3 + 12, boxes.y + boxes.h + 4, "WAITING " + ui::fixed(static_cast<double>(hs.waiting) / sr, 2) + " S", ui::red);
    }

    // The last steps, oldest first.
    ui::fill(*canvas, steps, t.panel);
    using Step = models::Handshake_frame::Step;
    constexpr auto size = models::Handshake_frame::size;
    auto y = steps.y + 8;
    for (auto i = size_t{}; i < size; ++i) {
        const auto& s = hs.steps[(hs.next + i) % size];
        if (s.seq == 0 || y + 10 > steps.y + steps.h) continue;
        auto line = ui::pad("#" + std::to_string(s.seq), 5) + "  " + ui::pad(std::to_string(s.frame), 10) + "  ";
        auto color = t.text;
        switch (s.kind) {
            case Step::Kind::Configure: line += "CONFIGURE AT " + ms(s.samples, sr); color = t.dim; break;
            case Step::Kind::Propose: line += "PROPOSE " + ms(s.samples, sr); color = ui::red; break;
            case Step::Kind::Accept: line += "ACCEPT " + ms(s.samples, sr) + " AFTER " + ui::fixed(static_cast<double>(s.waited) / sr, 2) + " S"; color = ui::green; break;
            case Step::Kind::Hard: line += "RESET HARD"; color = t.dim; break;
        }
        ui::text(*canvas, steps.x + 8, y, line, color);
        y += 14;
    }

    canvas->restore();
}

} // namespace tiny::edit
