#include "custom_view.h"

#include <variant>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

namespace tiny {

auto Custom_view::on_draw(App_state& app_state) -> void
{
    using enum Param_model::Param_id;
    using enum Param_model::Export_id;

    auto& params_state = app_state.params_state;
    auto& view_context = app_state.view_context;
    auto& actions = app_state.action_receiver;

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

    const auto g = params[enum_raw(gain)];
    const auto pk_x = exports[enum_raw(peak_in)];
    const auto pk_y = exports[enum_raw(peak_out)];

    // Get incremented parameter value from a drag.
    auto get_next = [=, this](auto x, auto& d) -> double {
        const auto drag_y = d.tpos.y - d.fpos.y;
        const auto drag_dy = drag_y - _ldrag;
        const auto norm_dy = drag_dy / lsize.h;
        return std::clamp(x + norm_dy, double{}, double{1});
    };

    // Handle user actions.
    std::visit(Inline_visitor{
        [&](const Drag_start& s) {
            _ldrag = {};
            const auto to_set = get_next(g, s);
            const auto id = enum_raw(gain);
            actions.add_action(Action_start{id});
            actions.add_action(Set_param{id, to_set});
            _ldrag = s.tpos.y - s.fpos.y;
        },
        [&](const Drag& s) {
            const auto to_set = get_next(g, s);
            const auto id = enum_raw(gain);
            actions.add_action(Set_param{id, to_set});
            _ldrag = s.tpos.y - s.fpos.y;
        },
        [&](const Drag_end& s) {
            const auto to_set = get_next(g, s);
            const auto id = enum_raw(gain);
            actions.add_action(Set_param{id, to_set});
            actions.add_action(Action_end{id});
            _ldrag = {};
        },
        [](const auto&) {}
    }, interaction.state);

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, rsize.w, rsize.h), paint);

    paint.setColor(SK_ColorBLACK);
    const auto div = rsize.w / 3;

    // Draw gain value.
    const auto g_h = g * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, div, g_h), paint);

    // Draw peak meters.
    const auto in_h = pk_x * rsize.h;
    const auto in_y = rsize.h - in_h;
    canvas->drawRect(SkRect::MakeXYWH(div, in_y, div, in_h), paint);

    const auto out_h = pk_y * rsize.h;
    const auto out_y = rsize.h - out_h;
    canvas->drawRect(SkRect::MakeXYWH(2 * div, out_y, div, out_h), paint);
}

} // namespace tiny