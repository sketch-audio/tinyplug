#include "editor.hpp"

#include "include/core/SkCanvas.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

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

// A 3x5 block glyph per character, drawn as rects. Enough for a dBFS readout and
// nothing more — the examples still have no font, and this deliberately does not
// give them one. Rows are top to bottom, three bits each.
struct Glyph { char ch; std::array<uint8_t, 5> rows; };
constexpr auto glyphs = std::array<Glyph, 17>{{
    {'0', {0b111, 0b101, 0b101, 0b101, 0b111}},
    {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
    {'2', {0b111, 0b001, 0b111, 0b100, 0b111}},
    {'3', {0b111, 0b001, 0b111, 0b001, 0b111}},
    {'4', {0b101, 0b101, 0b111, 0b001, 0b001}},
    {'5', {0b111, 0b100, 0b111, 0b001, 0b111}},
    {'6', {0b111, 0b100, 0b111, 0b101, 0b111}},
    {'7', {0b111, 0b001, 0b001, 0b001, 0b001}},
    {'8', {0b111, 0b101, 0b111, 0b101, 0b111}},
    {'9', {0b111, 0b101, 0b111, 0b001, 0b111}},
    {'-', {0b000, 0b000, 0b111, 0b000, 0b000}},
    {'.', {0b000, 0b000, 0b000, 0b000, 0b010}},
    {'I', {0b111, 0b010, 0b010, 0b010, 0b111}},
    {'N', {0b101, 0b111, 0b111, 0b111, 0b101}},
    {'F', {0b111, 0b100, 0b111, 0b100, 0b100}},
    {'d', {0b001, 0b001, 0b111, 0b101, 0b111}},
    {'B', {0b110, 0b101, 0b110, 0b101, 0b110}},
}};

// Returns the width drawn, so callers can right-align without measuring twice.
auto draw_text(SkCanvas& canvas, float x, float y, float px, std::string_view text, SkColor color) -> float
{
    auto pen = x;
    for (const auto ch : text) {
        const auto it = std::find_if(glyphs.begin(), glyphs.end(),
                                     [ch](const Glyph& g) { return g.ch == ch; });
        if (it == glyphs.end()) { pen += px * 2.f; continue; } // Unknown: a space.
        for (auto row = 0; row < 5; ++row) {
            for (auto col = 0; col < 3; ++col) {
                if ((it->rows[static_cast<size_t>(row)] >> (2 - col)) & 1) {
                    fill(canvas, pen + static_cast<float>(col) * px,
                         y + static_cast<float>(row) * px, px, px, color);
                }
            }
        }
        pen += px * 4.f; // Three columns plus one of tracking.
    }
    return pen - x;
}

// Peak as dBFS to two decimals, which is the resolution a host meter's own readout
// tends to show, so the two can be compared digit for digit.
auto peak_db_text(double plain) -> std::string
{
    if (!(plain > 0.)) return "-INF dB";
    const auto db = 20. * std::log10(plain);
    if (db <= -99.99) return "-INF dB";

    // Manual, to avoid dragging <format>/printf into an example editor.
    const auto hundredths = std::lround(std::abs(db) * 100.);
    auto out = std::string{};
    if (db < 0. && hundredths != 0) out += '-';
    out += std::to_string(hundredths / 100);
    out += '.';
    const auto frac = hundredths % 100;
    if (frac < 10) out += '0';
    out += std::to_string(frac);
    out += " dB";
    return out;
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

    // Logical first, scaled second: the canvas is in physical pixels but pointer
    // positions are logical, so the hit frame below has to be built from these.
    constexpr auto pad_l = 16.f;
    constexpr auto row_h_l = 52.f;
    constexpr auto bar_h_l = 26.f;
    constexpr auto swatch_l = 26.f;

    const auto pad = pad_l * scale;
    const auto row_h = row_h_l * scale;
    const auto bar_h = bar_h_l * scale;
    const auto swatch = swatch_l * scale;
    const auto track = _dark ? SkColorSetRGB(44, 44, 52) : SkColorSetRGB(216, 216, 224);

    // --- Row 1's latch. Click anywhere in its strip to clear the hold. ---
    const auto peak_strip = Frame{
        .x = pad_l,
        .y = pad_l,
        .w = static_cast<double>(view_context.logical_size.w) - 2. * pad_l,
        .h = row_h_l
    };
    for (const auto& event : view_context.interaction.events.events) {
        if (const auto* down = std::get_if<Pointer_down>(&event.event)) {
            if (down->button == Pointer_button::left && peak_strip.contains(down->pos)) {
                _peak_hold = 0.;
            }
        }
    }
    _peak_hold = std::max(_peak_hold, meters[enum_raw(Meter::peak_in)]);

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

        // Row 1 only: a hold marker on the bar, and the latched peak as dBFS in the
        // gap beneath it. The bar itself stays instantaneous, so the two together
        // show both what is arriving now and the highest thing that ever did.
        if (address == enum_raw(Meter::peak_in)) {
            const auto hold_norm = span > 0. ? (_peak_hold - range.min_val) / span : 0.;
            const auto hold_x = bar_x + bar_w * static_cast<float>(std::clamp(hold_norm, 0., 1.));
            fill(*canvas, std::min(hold_x, bar_x + bar_w - 2.f * scale), y,
                 2.f * scale, bar_h, row_colors[0]);

            draw_text(*canvas, bar_x, y + bar_h + 4.f * scale, 3.f * scale,
                      peak_db_text(_peak_hold), row_colors[0]);
        }
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
