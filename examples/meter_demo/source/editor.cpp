#include "editor.hpp"

#include "include/core/SkCanvas.h"

namespace tiny::plugin {

namespace {

// Row colours, in declaration order. See README.md — there is no text rendering
// in tinyplug's examples, so the rows are identified by position and colour.
constexpr auto row_colors = std::array<SkColor, 4>{
    SkColorSetRGB(232,  93,  93),  // peak_in       — Peak
    SkColorSetRGB( 96, 200, 120),  // stream_lfo    — Stream, moving
    SkColorSetRGB( 90, 150, 235),  // stream_const  — Stream, constant
    SkColorSetRGB(230, 190,  80),  // stream_sparse — Stream, sparse
};
constexpr auto trig_color = SkColorSetRGB(200, 110, 220);

auto fill(SkCanvas& canvas, float x, float y, float w, float h, SkColor color) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    canvas.drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

} // namespace

auto Editor::notify(const Host_event& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view_context = state.view_context;
    auto* canvas = view_context.canvas;
    if (!canvas) return;

    const auto scale = static_cast<float>(view_context.scale);
    const auto w = static_cast<float>(view_context.logical_size.w) * scale;
    const auto h = static_cast<float>(view_context.logical_size.h) * scale;

    const auto now = view_context.time_now;
    const auto dt = _has_time ? Durations::delta_secs(_last_time, now) : 0.;
    _last_time = now;
    _has_time = true;

    fill(*canvas, 0, 0, w, h, _dark ? SkColorSetRGB(22, 22, 26) : SkColorSetRGB(244, 244, 248));

    const auto meters = state.processor_state.meters;
    if (meters.size() < num_meters) return;

    const auto pad = 16.f * scale;
    const auto row_h = 52.f * scale;
    const auto bar_h = 26.f * scale;
    const auto swatch = 26.f * scale;
    const auto track = _dark ? SkColorSetRGB(44, 44, 52) : SkColorSetRGB(216, 216, 224);

    // --- The four level rows. Each bar is the value normalized against the
    // meter's own declared Range, so every row reads 0..1 on screen. ---
    for (auto row = 0; row < 4; ++row) {
        const auto address = static_cast<uint32_t>(row);
        const auto& range = User_meters::spec(address).range;
        const auto span = range.max_val - range.min_val;
        const auto norm = span > 0. ? (meters[address] - range.min_val) / span : 0.;
        const auto clamped = static_cast<float>(std::clamp(norm, 0., 1.));

        const auto y = pad + static_cast<float>(row) * row_h;
        fill(*canvas, pad, y, swatch, swatch, row_colors[static_cast<size_t>(row)]);

        const auto bar_x = pad + swatch + pad;
        const auto bar_w = w - bar_x - pad;
        fill(*canvas, bar_x, y, bar_w, bar_h, track);
        fill(*canvas, bar_x, y, bar_w * clamped, bar_h, row_colors[static_cast<size_t>(row)]);
    }

    // --- The trigger row. ---
    // `run_frame` surfaces a Trig for exactly one frame, so a non-zero value here
    // is one event. Recording the magnitudes in arrival order is what makes the
    // two failure modes visible: the processor emits 1,2,3..8 and repeats, so a
    // phantom trigger shows as a repeated step and a swallowed one as a gap.
    const auto trig_address = static_cast<uint32_t>(Meter::trig_pulse);
    const auto trig_value = meters[trig_address];
    if (trig_value > 0.) {
        _trig_sequence[_trig_write] = static_cast<uint32_t>(trig_value + 0.5);
        _trig_write = (_trig_write + 1) % _trig_sequence.size();
        _trig_flash = 0.15;
    }
    _trig_flash = std::max(0., _trig_flash - dt);

    const auto trig_y = pad + 4.f * row_h;
    const auto lamp = _trig_flash > 0. ? trig_color : track;
    fill(*canvas, pad, trig_y, swatch, swatch, lamp);

    // Eight history steps, oldest on the left, height proportional to magnitude.
    // Correct behaviour draws a clean repeating staircase.
    const auto hist_x = pad + swatch + pad;
    const auto hist_w = w - hist_x - pad;
    const auto step_w = hist_w / static_cast<float>(_trig_sequence.size());
    const auto hist_h = 72.f * scale;
    fill(*canvas, hist_x, trig_y, hist_w, hist_h, track);

    for (auto i = size_t{}; i < _trig_sequence.size(); ++i) {
        // Draw oldest-first so the staircase reads left to right.
        const auto slot = (_trig_write + i) % _trig_sequence.size();
        const auto magnitude = _trig_sequence[slot];
        if (magnitude == 0) continue;
        const auto frac = static_cast<float>(magnitude) / 8.f;
        const auto bar = hist_h * frac;
        fill(*canvas, hist_x + static_cast<float>(i) * step_w + step_w * 0.15f,
             trig_y + hist_h - bar, step_w * 0.7f, bar, trig_color);
    }
}

} // namespace tiny::plugin
