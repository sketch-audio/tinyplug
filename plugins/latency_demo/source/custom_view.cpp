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
    auto& params = params_state.params;
    auto& exports = params_state.exports;

    const auto id = enum_raw(Param_id::latency_mode);
    const auto curr = params[id];

    // Handle user actions.
    std::visit(Inline_visitor{
        [&](const Click&) {
            _actions.push(Action_start{id});
            _actions.push(Set_param{id, curr == 0 ? double{1} : double{0}}); // Toggle.
            _actions.push(Action_end{id});
        },
        [](const auto&) {}
    }, interaction.state);

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(curr == 0 ? SK_ColorGREEN : SK_ColorYELLOW);
    if (curr != exports[enum_raw(Export_id::latency_actual)]) {
        paint.setColor(SK_ColorRED);
    }

    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);
}

} // namespace tiny