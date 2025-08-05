#pragma once

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

struct Custom_view {

    // Here you have access to the user's interaction state, draw context, etc.
    auto on_draw(View_context& context) -> void
    {
        auto* canvas = context.draw_context.canvas;
        const auto size = context.draw_context.size;

        auto paint = SkPaint{};
        paint.setColor(SK_ColorWHITE);
        paint.setStyle(SkPaint::kFill_Style);
        canvas->drawRect(SkRect::MakeXYWH(0, 0, size.width, size.height), paint);

        // Get param, peak values.
        auto& params = context.params_state.params;
        const auto gain_plain = params[enum_raw(Param_id::gain)];

        auto& exports = context.params_state.exports;
        const auto peak_in = exports[enum_raw(Export_id::peak_in)];
        const auto peak_out = exports[enum_raw(Export_id::peak_out)];

        paint.setColor(SK_ColorBLACK);
        const auto div = size.width / 3;

        // Draw gain value.
        const auto g_h = gain_plain * size.height;
        const auto g_y = size.height - g_h;
        canvas->drawRect(SkRect::MakeXYWH(0, g_y, div, g_h), paint);

        // Draw peak meters.
        const auto in_h = peak_in * size.height;
        const auto in_y = size.height - in_h;
        canvas->drawRect(SkRect::MakeXYWH(div, in_y, div, in_h), paint);

        const auto out_h = peak_out * size.height;
        const auto out_y = size.height - out_h;
        canvas->drawRect(SkRect::MakeXYWH(2 * div, out_y, div, out_h), paint);
    }

private:

    using User_params = Param_infos<Param_model>;
    using Param_id = Param_model::Param_id;
    using Export_id = Param_model::Export_id;

    User_params _params{};

};

} // namespace tiny