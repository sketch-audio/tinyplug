#include "editor.hpp"

#include "include/core/SkCanvas.h"

namespace tiny::plugin {

auto Editor::on_gui_create(Gui_info) -> void
{
}

auto Editor::on_gui_show() -> void
{
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& processor_state = state.processor_state;
    auto& view_context = state.view_context;

    auto* canvas = view_context.canvas;

    // Resolve the real size.
    const auto lsize = view_context.logical_size;
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Get param values.
    auto& params = processor_state.params;

    const auto id = enum_raw(Address::Gain);
    const auto g = static_cast<float>(params[id]);

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(_dark ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);

    // Draw gain value.
    paint.setColor(_dark ? SK_ColorWHITE : SK_ColorBLACK);
    const auto g_h = g * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, static_cast<float>(rsize.w), g_h), paint);
}

auto Editor::notify(const Host_event& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Editor::on_gui_hide() -> void
{
}

auto Editor::on_gui_destroy() -> void
{
}

} // namespace tiny::plugin