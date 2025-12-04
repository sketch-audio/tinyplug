#include "plug_editor.h"

#include "include/core/SkCanvas.h"

namespace tiny {

auto Plug_editor::on_gui_show(const Edit_context& edit) -> void
{
    _edit = edit;
}

auto Plug_editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& processor_state = state.processor_state;
    auto& view_context = state.view_context;

    auto& interaction = view_context.interaction;
    auto* canvas = view_context.canvas;

    // Resolve the real size.
    const auto lsize = view_context.logical_size;
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Get param, export values.
    auto& params = processor_state.params;
    auto& meters = processor_state.meters;

    const auto id = enum_raw(Param_address::latency_mode);
    const auto curr = params[id];

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(curr == 0 ? SK_ColorGREEN : SK_ColorYELLOW);
    if (curr != meters[enum_raw(Meter_address::latency_actual)]) {
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

} // namespace tiny