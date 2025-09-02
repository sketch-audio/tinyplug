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

    const auto id = enum_raw(Param_id::gain);
    const auto g = params[id];

    // Get incremented parameter value from a drag.
    auto get_next = [=, this](auto x, auto& d) -> double {
        const auto drag_y = d.tpos.y - d.fpos.y;
        const auto drag_dy = drag_y - _ldrag;
        const auto norm_dy = drag_dy / lsize.h;
        return std::clamp(x - norm_dy, double{}, double{1});
    };

    // Handle user actions.
    std::visit(Inline_visitor{
        [&](const Drag_start& s) {
            _ldrag = {};
            const auto to_set = get_next(g, s);
            _actions.push(Action_start{id});
            _actions.push(Set_param{id, to_set});
            _ldrag = s.tpos.y - s.fpos.y;
        },
        [&](const Drag& s) {
            const auto to_set = get_next(g, s);
            _actions.push(Set_param{id, to_set});
            _ldrag = s.tpos.y - s.fpos.y;
        },
        [&](const Drag_end& s) {
            const auto to_set = get_next(g, s);
            _actions.push(Set_param{id, to_set});
            _actions.push(Action_end{id});
            _ldrag = {};
        },
        [](const auto&) {}
    }, interaction.state);
    track_is_down(interaction.state, _down); // Track down.

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(view_context.dark_mode ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, rsize.w, rsize.h), paint);

    // Draw gain value.
    paint.setColor(view_context.dark_mode ? SK_ColorWHITE : SK_ColorBLACK);
    if (_down) {
        paint.setColor(SK_ColorBLUE);
    }

    const auto g_h = g * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, rsize.w, g_h), paint);
}

} // namespace tiny