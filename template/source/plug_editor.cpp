#include "plug_editor.h"

#include "include/core/SkCanvas.h"

namespace tiny {

auto Plug_editor::on_gui_create() -> void
{
}

auto Plug_editor::on_gui_show(const Edit_context& edit) -> void
{
    _edit = edit;

    auto make_translation = [](Coords fpos, Coords tpos) -> Coords {
        return {tpos.x - fpos.x, tpos.y - fpos.y};
    };

    auto resolve_deltas = [this](Coords translation, Frame frame) -> Coords {
        const auto tx = translation.x - _translation.x;
        const auto ty = translation.y - _translation.y;

        const auto sx = frame.w;
        const auto sy = frame.h;

        const auto dx = tx / sx;
        const auto dy = ty / sy;

        _translation = translation; // Update translation!!!

        return {dx, dy};
    };

    auto resolve_incremented = [](double knob_value, Coords deltas) -> double {
        const auto next = knob_value - deltas.y;
        const auto clamped = std::clamp(next, 0., 1.);
        return clamped;
    };

    _drag = std::make_unique<Drag_recognizer>(Gesture_callbacks<Drag_info>{
        .on_started = [=, this](const Drag_info& info) {
            _translation = {};
            const auto translation = make_translation(info.fpos, info.tpos);
            const auto deltas = resolve_deltas(translation, _frame);
            const auto value = resolve_incremented(_value, deltas);
            const auto addr = enum_raw(Param_address::gain);
            _edit.actions.push(Action_start{addr});
            _edit.actions.push(Set_param{addr, value});
        },
        .on_updated = [=, this](const Drag_info& info) {
            const auto translation = make_translation(info.fpos, info.tpos);
            const auto deltas = resolve_deltas(translation, _frame);
            const auto value = resolve_incremented(_value, deltas);
            const auto addr = enum_raw(Param_address::gain);
            _edit.actions.push(Set_param{addr, value});
        },
        .on_ended = [=, this](const Drag_info& info) {
            const auto translation = make_translation(info.fpos, info.tpos);
            const auto deltas = resolve_deltas(translation, _frame);
            const auto value = resolve_incremented(_value, deltas);
            const auto addr = enum_raw(Param_address::gain);
            _edit.actions.push(Set_param{addr, value});
            _edit.actions.push(Action_end{addr});
        },
        .on_cancelled = [&]() { _translation = {}; }
    });
}

auto Plug_editor::on_gui_draw(Plugin_state& state) -> void
{
    // Update (Send actions).
    auto& view_context = state.view_context;
    const auto lsize = view_context.logical_size;
    auto frame = Frame{.x = 0, .y = 0, .w = static_cast<double>(lsize.w), .h = static_cast<double>(lsize.h)};
    if (_frame != frame) {
        _frame = frame;
        if (_drag)
            _drag->set_frame(_frame);
    }

    const auto& param_values = state.processor_state.params;
    const auto addr = enum_raw(Param_address::gain);
    _value = param_values[addr];
    if (_drag)
        _drag->process_events(view_context.interaction.events);

    // Draw.
    auto* canvas = view_context.canvas;

    // Calculate real size.
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(_dark ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);

    // Draw gain value.
    paint.setColor(_dark ? SK_ColorWHITE : SK_ColorBLACK);
    const auto g_h = _value * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, static_cast<float>(rsize.w), g_h), paint);
}

auto Plug_editor::on_gui_notify(const Ui_notification& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Plug_editor::on_gui_hide() -> void
{
}

auto Plug_editor::on_gui_destroy() -> void
{
}

} // namespace tiny