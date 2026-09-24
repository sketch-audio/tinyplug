#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#include <tinyplug/tinyplug.hpp>

#include <tiny_ui/controls.hpp>

// Drawing for simple editors: a theme, rects, a 3x5 pixel font, and the controls from
// controls.hpp. Logical coordinates throughout; scale the canvas by the view's scale first.
// Include from .cpp files only: Skia is private to a plug-in's library.
namespace tiny::ui {

// MARK: - colour

struct Theme {
    SkColor background{};
    SkColor panel{};
    SkColor track{};
    SkColor text{};
    SkColor dim{};
};

inline auto theme(bool dark) -> Theme
{
    if (dark) return {SkColorSetRGB(22, 22, 26), SkColorSetRGB(32, 32, 38), SkColorSetRGB(48, 48, 56),
                      SkColorSetRGB(225, 225, 232), SkColorSetRGB(120, 120, 132)};
    return {SkColorSetRGB(244, 244, 248), SkColorSetRGB(232, 232, 238), SkColorSetRGB(212, 212, 222),
            SkColorSetRGB(30, 30, 36), SkColorSetRGB(130, 130, 142)};
}

inline constexpr auto red = SkColorSetRGB(232, 93, 93);
inline constexpr auto amber = SkColorSetRGB(230, 190, 80);
inline constexpr auto green = SkColorSetRGB(96, 200, 120);
inline constexpr auto blue = SkColorSetRGB(90, 150, 235);
inline constexpr auto purple = SkColorSetRGB(200, 110, 220);

inline auto mix(SkColor a, SkColor b, float t) -> SkColor
{
    const auto lerp = [t](U8CPU x, U8CPU y) { return static_cast<U8CPU>(static_cast<float>(x) + t * (static_cast<float>(y) - static_cast<float>(x))); };
    return SkColorSetRGB(lerp(SkColorGetR(a), SkColorGetR(b)), lerp(SkColorGetG(a), SkColorGetG(b)), lerp(SkColorGetB(a), SkColorGetB(b)));
}

// MARK: - shapes

inline auto rect(const Frame& f) -> SkRect
{
    return SkRect::MakeXYWH(static_cast<float>(f.x), static_cast<float>(f.y), static_cast<float>(f.w), static_cast<float>(f.h));
}

inline auto fill(SkCanvas& canvas, const Frame& f, SkColor color) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setAntiAlias(true);
    canvas.drawRect(rect(f), paint);
}

inline auto outline(SkCanvas& canvas, const Frame& f, SkColor color, float width = 1.f) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setAntiAlias(true);
    canvas.drawRect(rect(f), paint);
}

inline auto line(SkCanvas& canvas, Coords a, Coords b, SkColor color, float width = 1.f) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setStrokeWidth(width);
    paint.setAntiAlias(true);
    canvas.drawLine(static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(b.x), static_cast<float>(b.y), paint);
}

// Split a frame: `inset` all round, rows and columns of equal size with `gap` between.
inline auto inset(const Frame& f, double by) -> Frame { return {f.x + by, f.y + by, f.w - 2 * by, f.h - 2 * by}; }

inline auto row(const Frame& f, int index, int count, double gap) -> Frame
{
    const auto h = (f.h - gap * (count - 1)) / count;
    return {f.x, f.y + index * (h + gap), f.w, h};
}

inline auto column(const Frame& f, int index, int count, double gap) -> Frame
{
    const auto w = (f.w - gap * (count - 1)) / count;
    return {f.x + index * (w + gap), f.y, w, f.h};
}

// MARK: - text

// 3x5 glyphs, rows top to bottom, three bits each. Lower case draws as upper case.
namespace detail {

struct Glyph { char ch; std::array<uint8_t, 5> rows; };

inline constexpr auto glyphs = std::array<Glyph, 63>{{
    {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}}, {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}},
    {'E', {7, 4, 6, 4, 7}}, {'F', {7, 4, 6, 4, 4}}, {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}},
    {'I', {7, 2, 2, 2, 7}}, {'J', {1, 1, 1, 5, 2}}, {'K', {5, 5, 6, 5, 5}}, {'L', {4, 4, 4, 4, 7}},
    {'M', {5, 7, 7, 5, 5}}, {'N', {6, 5, 5, 5, 5}}, {'O', {2, 5, 5, 5, 2}}, {'P', {6, 5, 6, 4, 4}},
    {'Q', {2, 5, 5, 6, 3}}, {'R', {6, 5, 6, 5, 5}}, {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}},
    {'U', {5, 5, 5, 5, 7}}, {'V', {5, 5, 5, 5, 2}}, {'W', {5, 5, 7, 7, 5}}, {'X', {5, 5, 2, 5, 5}},
    {'Y', {5, 5, 2, 2, 2}}, {'Z', {7, 1, 2, 4, 7}},
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}},
    {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}},
    {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}},
    {'-', {0, 0, 7, 0, 0}}, {'.', {0, 0, 0, 0, 2}}, {':', {0, 2, 0, 2, 0}}, {'+', {0, 2, 7, 2, 0}},
    {'/', {1, 1, 2, 4, 4}}, {'>', {4, 2, 1, 2, 4}}, {'<', {1, 2, 4, 2, 1}}, {'=', {0, 7, 0, 7, 0}},
    {'#', {5, 7, 5, 7, 5}}, {'%', {5, 1, 2, 4, 5}}, {'(', {1, 2, 2, 2, 1}}, {')', {4, 2, 2, 2, 4}},
    {'[', {3, 2, 2, 2, 3}}, {']', {6, 2, 2, 2, 6}}, {'_', {0, 0, 0, 0, 7}}, {',', {0, 0, 0, 2, 4}},
    {'!', {2, 2, 2, 0, 2}}, {'?', {6, 1, 2, 0, 2}}, {'*', {0, 5, 2, 5, 0}}, {'\'', {2, 2, 0, 0, 0}},
    {'|', {2, 2, 2, 2, 2}}, {'"', {5, 5, 0, 0, 0}}, {'^', {2, 5, 0, 0, 0}}, {'~', {0, 3, 6, 0, 0}},
    {'@', {7, 5, 7, 4, 7}}, {'$', {3, 6, 2, 3, 6}}, {'&', {2, 5, 2, 5, 3}},
}};

inline auto glyph(char ch) -> const Glyph*
{
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    const auto it = std::find_if(glyphs.begin(), glyphs.end(), [ch](const Glyph& g) { return g.ch == ch; });
    return it == glyphs.end() ? nullptr : &*it;
}

} // namespace detail

inline constexpr auto text_height(double px) -> double { return 5 * px; }
inline constexpr auto text_width(std::string_view text, double px) -> double { return text.empty() ? 0 : (4 * static_cast<double>(text.size()) - 1) * px; }

// Returns the width drawn. `px` is one font pixel; 2 reads comfortably.
inline auto text(SkCanvas& canvas, double x, double y, std::string_view str, SkColor color, double px = 2) -> double
{
    auto paint = SkPaint{};
    paint.setColor(color);
    auto pen = x;
    for (const auto ch : str) {
        if (const auto* g = detail::glyph(ch)) {
            for (auto r = 0; r < 5; ++r) {
                for (auto c = 0; c < 3; ++c) {
                    if ((g->rows[static_cast<size_t>(r)] >> (2 - c)) & 1) {
                        canvas.drawRect(SkRect::MakeXYWH(static_cast<float>(pen + c * px), static_cast<float>(y + r * px),
                                                         static_cast<float>(px), static_cast<float>(px)), paint);
                    }
                }
            }
        }
        pen += 4 * px;
    }
    return pen - x - px;
}

// Centred in `f`, vertically and horizontally.
inline auto text_in(SkCanvas& canvas, const Frame& f, std::string_view str, SkColor color, double px = 2) -> void
{
    text(canvas, f.x + (f.w - text_width(str, px)) / 2, f.y + (f.h - text_height(px)) / 2, str, color, px);
}

// `value` with `decimals` places, no locale, no <format>.
inline auto fixed(double value, int decimals) -> std::string
{
    if (!std::isfinite(value)) return value != value ? "NAN" : (value < 0 ? "-INF" : "INF");
    auto scale = 1.;
    for (auto i = 0; i < decimals; ++i) scale *= 10.;
    const auto scaled = std::llround(std::abs(value) * scale);
    auto out = std::string{value < 0 && scaled != 0 ? "-" : ""};
    out += std::to_string(scaled / static_cast<long long>(scale));
    if (decimals > 0) {
        auto frac = std::to_string(scaled % static_cast<long long>(scale));
        out += '.' + std::string(static_cast<size_t>(decimals) - frac.size(), '0') + frac;
    }
    return out;
}

// Right-aligned in `width` characters.
inline auto pad(std::string str, size_t width) -> std::string
{
    return str.size() >= width ? str : std::string(width - str.size(), ' ') + str;
}

inline auto db(double gain, int decimals = 1) -> std::string
{
    if (!(gain > 1e-5)) return "-INF";
    return fixed(20. * std::log10(gain), decimals);
}

// MARK: - meters

// Linear gain to 0…1 on a dB scale, `floor_db` at the bottom.
inline auto meter_norm(double gain, double floor_db = -60., double ceil_db = 12.) -> double
{
    if (!(gain > 0.)) return 0.;
    return std::clamp((20. * std::log10(gain) - floor_db) / (ceil_db - floor_db), 0., 1.);
}

// A vertical meter: level as a bar, green to amber at -6 dB and red past 0 dB, `hold` as a line,
// and a tick at 0 dB.
inline auto draw_meter(SkCanvas& canvas, const Theme& t, const Frame& f, double level, double hold) -> void
{
    fill(canvas, f, t.track);
    const auto n = meter_norm(level);
    const auto color = level > 1. ? red : level > 0.5 ? amber : green;
    fill(canvas, {f.x, f.y + f.h * (1 - n), f.w, f.h * n}, color);
    const auto h = f.y + f.h * (1 - meter_norm(hold));
    if (hold > 0.) fill(canvas, {f.x, h - 1, f.w, 2}, hold > 1. ? red : t.text);
    const auto unity = f.y + f.h * (1 - meter_norm(1.));
    fill(canvas, {f.x - 3, unity - 0.5, 3, 1}, t.dim);
}

// MARK: - controls

// A knob value as its parameter holds it: discrete parameters snap to their steps.
inline auto shown_knob(uint32_t address, double knob) -> double
{
    const auto& semantics = User_params::param_spec(address).semantics;
    if (!params::Value_helper::is_discrete(semantics)) return knob;
    return params::Value_helper::plain_to_knob(params::Value_helper::knob_to_plain(knob, semantics), semantics);
}

// Sliders fill left to right, faders bottom to top, stepped parameters in whole steps; toggles
// and choices light when on.
inline auto draw_controls(SkCanvas& canvas, const Theme& t, std::span<const Control> controls, std::span<const double> knobs) -> void
{
    for (const auto& c : controls) {
        const auto knob = shown_knob(c.address, std::clamp(knobs[c.address], 0., 1.));
        fill(canvas, c.frame, t.track);
        if (c.kind == Control::Kind::Toggle || c.kind == Control::Kind::Choice) {
            const auto on = c.kind == Control::Kind::Choice ? std::abs(knob - c.choice) < 1e-3 : knob >= 0.5;
            if (on) fill(canvas, c.frame, c.color);
            text_in(canvas, c.frame, c.label, on ? t.background : t.text);
            continue;
        }
        if (c.kind == Control::Kind::Fader) {
            fill(canvas, {c.frame.x, c.frame.y + c.frame.h * (1 - knob), c.frame.w, c.frame.h * knob}, c.color);
            text_in(canvas, {c.frame.x, c.frame.y + c.frame.h - 16, c.frame.w, 12}, c.label, t.text);
            continue;
        }
        fill(canvas, {c.frame.x, c.frame.y, c.frame.w * knob, c.frame.h}, c.color);
        text(canvas, c.frame.x + 6, c.frame.y + (c.frame.h - text_height(2)) / 2, c.label, t.text);
    }
}

} // namespace tiny::ui
