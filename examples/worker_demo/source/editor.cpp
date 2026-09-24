#include "editor.hpp"

#include <tiny_ui/draw.hpp>

#include <chrono>
#include <string>

namespace tiny::edit {

namespace {

constexpr auto ping_period_ns = int64_t{250'000'000};

auto steady_ns() -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

auto Editor::notify(const Host_event& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Editor::on_worker_reply(const User_work::To_editor& reply) -> void
{
    std::visit(Inline_visitor{
        [this](const models::Pong& p) {
            _pong_ms = static_cast<double>(steady_ns() - p.sent_ns) / 1e6;
            _designs = p.designs;
            ++_legs[1].count;
            _legs[1].flash = 0.2;
        },
    }, reply);
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view = state.view_context;
    auto* canvas = view.canvas;
    if (!canvas) return;

    const auto dt = _last == Time_point{} ? 0. : Durations::delta_secs(_last, view.time_now);
    _last = view.time_now;

    // Pings go out on a timer while the window is open.
    const auto now = steady_ns();
    if (now >= _next_ping_ns && _worker.push(models::Ping{++_ping_seq, now})) {
        _next_ping_ns = now + ping_period_ns;
        ++_legs[0].count;
        _legs[0].flash = 0.2;
    }

    // The processor's legs, from its frame: a lamp whenever a count moved.
    const auto& ch = state.processor_state.blocks.latest<Block::Channel>();
    const auto observe = [&](Leg& leg, uint32_t count) {
        if (count != leg.count) leg.flash = 0.2;
        leg.count = count;
    };
    observe(_legs[2], ch.heartbeats + ch.designs);
    observe(_legs[3], ch.echoes + ch.curves);
    for (auto& leg : _legs) leg.flash = std::max(0., leg.flash - dt);

    const auto t = ui::theme(_dark);
    const auto& knobs = state.processor_state.params;
    const auto bounds = ui::inset({0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, 16);
    const auto diagram = Frame{bounds.x, bounds.y, bounds.w, 170};
    const auto curve = Frame{bounds.x, diagram.y + diagram.h + 16, 200, bounds.y + bounds.h - (diagram.y + diagram.h + 16)};
    const auto side = Frame{curve.x + curve.w + 16, curve.y, bounds.w - curve.w - 16, curve.h};
    const auto drive = Frame{side.x, side.y + side.h - 26, side.w, 26};

    const auto controls = std::array<ui::Control, 1>{{{drive, enum_raw(Address::Drive), ui::Control::Kind::Slider, "DRIVE", ui::amber}}};
    _controls.interact(view.interaction, _edit.actions, controls, knobs);

    canvas->save();
    canvas->scale(static_cast<float>(view.scale), static_cast<float>(view.scale));
    ui::fill(*canvas, {0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, t.background);

    // Processor | worker | editor, with a lane each way between neighbours.
    const auto box_w = 110.;
    const auto gap = (diagram.w - 3 * box_w) / 2;
    const auto box = [&](int i) { return Frame{diagram.x + i * (box_w + gap), diagram.y + 40, box_w, 90}; };
    const auto names = std::array<const char*, 3>{"PROCESSOR", "WORKER", "EDITOR"};
    for (auto i = 0; i < 3; ++i) {
        ui::fill(*canvas, box(i), t.panel);
        ui::text_in(*canvas, box(i), names[static_cast<size_t>(i)], t.text);
    }
    const auto lane = [&](int between, bool upper, const Leg& leg, bool rightwards, const std::string& label) {
        const auto a = box(between);
        const auto x0 = a.x + a.w + 6;
        const auto x1 = a.x + a.w + gap - 6;
        const auto y = a.y + (upper ? 26 : 64);
        const auto color = leg.flash > 0 ? ui::green : t.dim;
        ui::line(*canvas, {x0, y}, {x1, y}, color, 2.f);
        const auto head = rightwards ? x1 : x0;
        const auto dir = rightwards ? -1. : 1.;
        ui::line(*canvas, {head, y}, {head + dir * 7, y - 5}, color, 2.f);
        ui::line(*canvas, {head, y}, {head + dir * 7, y + 5}, color, 2.f);
        ui::text(*canvas, x0, upper ? y - 30 : y + 10, label, t.text);
    };
    const auto ms = [&](double frames) { return ui::fixed(frames * 1000. / (ch.sr > 0 ? ch.sr : 48000.), 1) + " MS"; };
    lane(0, true, _legs[2], true, "BEATS " + std::to_string(ch.heartbeats) + "  DESIGNS " + std::to_string(ch.designs));
    lane(0, false, _legs[3], false, "ECHOES " + std::to_string(ch.echoes) + "  CURVES " + std::to_string(ch.curves));
    lane(1, true, _legs[1], true, "PONGS " + std::to_string(_legs[1].count));
    lane(1, false, _legs[0], false, "PINGS " + std::to_string(_legs[0].count));
    ui::text(*canvas, diagram.x, diagram.y, "ROUND TRIPS: PROCESSOR " + ms(static_cast<double>(ch.round_trip)) + "   EDITOR " + ui::fixed(_pong_ms, 1) + " MS", t.dim);

    // The curve the processor shapes with; amber until it matches the Drive asked for.
    const auto spec = User_params::param_spec(enum_raw(Address::Drive)).semantics;
    const auto wanted = params::Value_helper::knob_to_plain(knobs[enum_raw(Address::Drive)], spec);
    const auto current = std::abs(ch.drive - wanted) < 1e-6;
    ui::fill(*canvas, curve, t.panel);
    ui::line(*canvas, {curve.x, curve.y + curve.h / 2}, {curve.x + curve.w, curve.y + curve.h / 2}, t.track);
    ui::line(*canvas, {curve.x + curve.w / 2, curve.y}, {curve.x + curve.w / 2, curve.y + curve.h}, t.track);
    const auto at = [&](size_t i) {
        const auto x = static_cast<double>(i) / static_cast<double>(models::curve_points - 1);
        return Coords{curve.x + x * curve.w, curve.y + curve.h * (0.5 - 0.45 * static_cast<double>(ch.table[i]))};
    };
    for (auto i = size_t{1}; i < models::curve_points; ++i) ui::line(*canvas, at(i - 1), at(i), current ? ui::green : ui::amber, 2.f);

    ui::text(*canvas, side.x, side.y, "CURVE DRIVE " + ui::fixed(ch.drive, 2), current ? ui::green : ui::amber, 3);
    ui::text(*canvas, side.x, side.y + 24, "DESIGNED BY THE WORKER: " + std::to_string(_designs), t.dim);
    ui::draw_controls(*canvas, t, controls, knobs);

    canvas->restore();
}

} // namespace tiny::edit
