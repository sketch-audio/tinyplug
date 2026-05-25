#include "plug_editor.h"

#include "include/core/SkCanvas.h"

namespace tiny {

auto Plug_editor::on_gui_create() -> void
{
}

auto Plug_editor::on_gui_show(const Edit_context& edit) -> void
{
    _edit = edit;
}

auto Plug_editor::on_gui_draw(Plugin_state& state) -> void
{
    auto* canvas = state.view_context.canvas;
    const auto lsize = state.view_context.logical_size;
    const auto scale = state.view_context.scale;
    const auto w = static_cast<float>(lsize.w * scale);
    const auto h = static_cast<float>(lsize.h * scale);

    auto paint = SkPaint{};
    paint.setColor(_dark ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, w, h), paint);
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
