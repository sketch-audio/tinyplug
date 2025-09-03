#include "custom_view.h"

#include <variant>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "platform/platform_view.h"

namespace tiny {

// MARK: - on create

auto Custom_view::on_create(const Action_queue::Receiver& actions, const Task_queue::Receiver& tasks) -> void
{
    _actions = actions;
    _task_receiver = tasks;
}

// MARK: - on draw

auto Custom_view::on_draw(App_state& app_state) -> void
{
    auto& params_state = app_state.params_state;
    auto& view_context = app_state.view_context;

    auto* canvas = view_context.canvas;

    // Resolve the real size.
    const auto lsize = view_context.logical_size;
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Get param values.
    auto& params = params_state.params;

    const auto id = enum_raw(Param_id::gain);
    const auto g = static_cast<float>(params[id]);

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(view_context.dark_mode ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);

    // Draw gain value.
    paint.setColor(view_context.dark_mode ? SK_ColorWHITE : SK_ColorBLACK);
    const auto g_h = g * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, static_cast<float>(rsize.w), g_h), paint);
}

} // namespace tiny