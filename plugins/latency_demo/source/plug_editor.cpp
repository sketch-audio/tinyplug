#include "plug_editor.h"

#include "include/core/SkCanvas.h"

namespace tiny {

auto Plug_editor::on_gui_create() -> void
{
}

auto Plug_editor::on_gui_show(const Edit_context& edit) -> void
{
    _edit = edit;

    _click = std::make_unique<Click_recognizer>(Gesture_callbacks<Click_info>{
        .on_started = [=, this](const Click_info& info) {
            const auto addr = enum_raw(Param_address::latency_mode);
            const auto next = (_value == 0) ? 1. : 0.;
            _edit.actions.push(Action_start{addr});
            _edit.actions.push(Set_param{addr, next});
            _edit.actions.push(Action_end{addr});
        },
        .on_updated = [](const Click_info&) {},
        .on_ended = [](const Click_info&) {},
        .on_cancelled = []() {}
    }, Click_recognizer::Desc{/* single, left click */});
}

auto Plug_editor::on_gui_draw(Plugin_state& state) -> void
{
    // Update (Send actions).
    auto& view_context = state.view_context;
    const auto lsize = view_context.logical_size;
    auto frame = Frame{.x = 0, .y = 0, .w = static_cast<double>(lsize.w), .h = static_cast<double>(lsize.h)};
    if (_frame != frame) {
        _frame = frame;
        if (_click)
            _click->set_frame(_frame);
    }

    const auto& param_values = state.processor_state.params;
    const auto addr = enum_raw(Param_address::latency_mode);
    _value = param_values[addr];

    if (_click)
        _click->process_events(view_context.interaction.events);

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
    paint.setColor(_value == 0 ? SK_ColorGREEN : SK_ColorYELLOW);

    const auto& meter_values = state.processor_state.meters;
    if (_value != meter_values[enum_raw(Meter_address::latency_actual)]) {
        paint.setColor(SK_ColorRED);
    }

    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);
}

auto Plug_editor::on_gui_notify(const Ui_notification& notification) -> void
{
    std::visit(Inline_visitor{
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