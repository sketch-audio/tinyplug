#include "plug_editor.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

namespace tiny {

auto Plug_editor::on_gui_show(const View_connection& connection) -> void
{
    _actions = connection.actions;
    _task_receiver = connection.tasks;
}

auto Plug_editor::on_gui_draw(App_state& app_state) -> void
{
    auto& processor_state = app_state.processor_state;
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
    auto& params = processor_state.params;
    auto& meters = processor_state.meters;

    const auto id = enum_raw(Param_address::latency_mode);
    const auto curr = params[id];

    for (auto& pointer : interaction.pointers) {
        // Handle user actions. (Should we bind on down?)
        std::visit(Inline_visitor{
            [&](const Click&) {
                _actions.push(Action_start{id});
                _actions.push(Set_param{id, curr == 0 ? double{1} : double{0}}); // Toggle.
                _actions.push(Action_end{id});
            },
            [](const auto&) {}
        }, pointer.state);
    }

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