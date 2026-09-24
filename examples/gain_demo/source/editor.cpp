#include "editor.hpp"

#include <tiny_ui/draw.hpp>

#include <algorithm>

namespace tiny::edit {

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
    const auto& meters = state.processor_state.meters;

    const auto bounds = ui::inset({0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, 16);
    const auto body = Frame{bounds.x, bounds.y + 28, bounds.w, bounds.h - 60};
    const auto bar = [&](int i) { return ui::column(body, i, 5, 10); }; // In L, In R, fader, Out L, Out R.
    const auto readout = [&](int i) { return Frame{bar(i).x - 4, body.y + body.h + 6, bar(i).w + 8, 10}; };
    const auto clear_area = Frame{body.x, body.y, body.w, body.h + 16}; // Bars and their readouts.

    const auto controls = std::array<ui::Control, 1>{{
        {bar(2), enum_raw(Address::Gain), ui::Control::Kind::Fader, "GAIN", ui::blue},
    }};
    _controls.interact(view.interaction, _edit.actions, controls, knobs);
    for (const auto& event : view.interaction.events.events) {
        if (const auto* down = std::get_if<Pointer_down>(&event.event); down && !event.consumed && clear_area.contains(down->pos)) _holds.fill(0.);
    }
    for (auto i = size_t{}; i < _holds.size() && i < meters.size(); ++i) _holds[i] = std::max(_holds[i], meters[i]);

    canvas->save();
    canvas->scale(static_cast<float>(view.scale), static_cast<float>(view.scale));
    ui::fill(*canvas, {0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, t.background);

    const auto& gain = User_params::param_spec(enum_raw(Address::Gain)).semantics;
    const auto plain = params::Value_helper::knob_to_plain(knobs[enum_raw(Address::Gain)], gain);
    ui::text_in(*canvas, {bounds.x, bounds.y, bounds.w, 16}, (plain > 1 ? "+" : "") + ui::db(plain) + " DB", t.text, 3);

    ui::draw_controls(*canvas, t, controls, knobs);
    for (auto i = 0; i < 4 && static_cast<size_t>(i) < meters.size(); ++i) {
        const auto column = bar(i < 2 ? i : i + 1);
        const auto hold = _holds[static_cast<size_t>(i)];
        ui::draw_meter(*canvas, t, column, meters[static_cast<size_t>(i)], hold);
        ui::text_in(*canvas, readout(i < 2 ? i : i + 1), ui::db(hold, 2), hold > 1. ? ui::red : t.text); // To compare with the host's peak readout.
    }
    ui::text_in(*canvas, {bar(0).x, body.y + body.h + 22, bar(1).x + bar(1).w - bar(0).x, 10}, "IN", t.dim);
    ui::text_in(*canvas, {bar(3).x, body.y + body.h + 22, bar(4).x + bar(4).w - bar(3).x, 10}, "OUT", t.dim);

    canvas->restore();
}

} // namespace tiny::edit
