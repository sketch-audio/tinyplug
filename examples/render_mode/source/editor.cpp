#include "editor.hpp"

#include "include/core/SkCanvas.h"

namespace tiny::plugin {

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view_context = state.view_context;
    const auto lsize = view_context.logical_size;
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Read the offline meter: 1 → offline (bounce), 0 → realtime.
    const auto& meter_values = state.processor_state.meters;
    const auto offline_addr = enum_raw(Meter::Offline);
    const auto is_offline = offline_addr < meter_values.size()
        && meter_values[offline_addr] > 0.5;

    // Solid fill: green for realtime, yellow for offline.
    auto paint = SkPaint{};
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(is_offline ? SK_ColorYELLOW : SK_ColorGREEN);

    auto* canvas = view_context.canvas;
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);
}

} // namespace tiny::plugin
