// tiny_text against Inter, a variable font: shaping kerns, features apply, the axes move advances, wrapping and
// ellipses, the instance cache quantizes, and embedded bytes round-trip. With TINY_TEXT_PNG set, also renders a
// specimen there (2x) to look at.

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

#include <audio_bench/audio_bench.hpp>
#include <embedded/fonts.hpp>
#include <tiny_text/fonts.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using namespace tiny::text;

auto faces() -> std::array<Face, 2>
{
    return {{
        {.family = "Inter", .data = embedded::fonts::intervariable_ttf()},
        {.family = "Inter", .data = embedded::fonts::intervariable_italic_ttf()},
    }};
}

auto style(float size, float weight = 400) -> Style { return {.family = "Inter", .size = size, .weight = weight}; }

// What a draw call put on a white raster, at 1x: the inked rows and columns.
struct Drawn {
    int top{-1}, bottom{-1}, left{-1}, right{-1};
    auto cy() const -> double { return (top + bottom + 1) / 2.; }
};

template<typename F>
auto drawn(int w, int h, F&& draw) -> Drawn
{
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    surface->getCanvas()->clear(SK_ColorWHITE);
    draw(*surface->getCanvas());
    auto pixmap = SkPixmap{};
    surface->peekPixels(&pixmap);
    auto d = Drawn{};
    for (auto y = 0; y < h; ++y) {
        for (auto x = 0; x < w; ++x) {
            if (SkColorGetR(pixmap.getColor(x, y)) > 128) continue; // Half-covered or more.
            if (d.top < 0) d.top = y;
            d.bottom = y;
            d.left = d.left < 0 ? x : std::min(d.left, x);
            d.right = std::max(d.right, x);
        }
    }
    return d;
}

auto render(const char* path) -> void
{
    auto fonts = Fonts{faces()};
    constexpr auto w = 640, h = 420;
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w * 2, h * 2));
    auto* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    canvas->scale(2, 2);
    const auto ink = SkColorSetRGB(24, 24, 28);
    auto y = 16.;
    for (auto wt = 100.f; wt <= 900.f; wt += 200.f) {
        fonts.line(*canvas, "Sphinx of black quartz, judge my vow", style(22, wt), {16, y, w - 32., 30}, ink);
        y += 30;
    }
    fonts.line(*canvas, "AVATAR Wave To Ty LT P. F,  0123456789", style(22, 500), {16, y, w - 32., 30}, ink);
    y += 34;
    fonts.line(*canvas, "Italic, 600", {.family = "Inter", .size = 18, .weight = 600, .italic = true}, {16, y, w - 32., 26}, ink);
    y += 34;
    const auto prose = "A variable font carries its whole design space in one file. HarfBuzz shapes each run against "
                       "the instance it will be drawn with, so kerning and advances follow the axes as they move.";
    const auto m = fonts.paragraph(*canvas, prose, {.family = "Inter", .size = 14, .line_height = 1.45f}, {16, y, 300, 0}, ink);
    fonts.paragraph(*canvas, prose, {.family = "Inter", .size = 14, .line_height = 1.45f}, {336, y, 288, 0}, ink, Align::Left, 3);
    y += m.height + 12;
    fonts.line(*canvas, "Small text at 9: the quick brown fox jumps over the lazy dog", style(9), {16, y, w - 32., 14}, ink);

    auto pixmap = SkPixmap{};
    if (!surface->peekPixels(&pixmap)) return;
    auto stream = SkFILEWStream{path};
    if (SkPngEncoder::Encode(&stream, pixmap, {})) std::printf("wrote %s\n", path);
}

} // namespace

auto main() -> int
{
    Tests::add("embedded bytes round-trip", [] {
        const auto data = embedded::fonts::intervariable_ttf();
        expect_true(data.size() == 879708, std::format("{} bytes", data.size()));
        expect_true(data[0] == std::byte{0} && data[1] == std::byte{1} && data[2] == std::byte{0} && data[3] == std::byte{0}, "a TrueType header");
    });

    Tests::add("shaping kerns: AV is narrower than A and V apart", [] {
        auto fonts = Fonts{faces()};
        const auto pair = fonts.measure("AV", style(40)).width;
        const auto apart = fonts.measure("A", style(40)).width + fonts.measure("V", style(40)).width;
        expect_true(pair < apart - 1, std::format("AV {:.2f}, A + V {:.2f}", pair, apart));
    });

    Tests::add("tnum makes figures one width", [] {
        auto fonts = Fonts{faces()};
        auto tabular = style(30);
        tabular.features[0] = {tag("tnum")};
        const auto ones = fonts.measure("1111", style(30)).width;
        const auto zeros = fonts.measure("0000", style(30)).width;
        expect_true(std::abs(ones - zeros) > 1, std::format("proportional: 1111 {:.2f}, 0000 {:.2f}", ones, zeros));
        const auto t_ones = fonts.measure("1111", tabular).width;
        const auto t_zeros = fonts.measure("0000", tabular).width;
        expect_true(std::abs(t_ones - t_zeros) < 0.01, std::format("tabular: 1111 {:.2f}, 0000 {:.2f}", t_ones, t_zeros));
    });

    Tests::add("wght moves advances", [] {
        auto fonts = Fonts{faces()};
        const auto regular = fonts.measure("Hamburgefonstiv", style(30, 400)).width;
        const auto bold = fonts.measure("Hamburgefonstiv", style(30, 800)).width;
        expect_true(bold > regular + 5, std::format("400 {:.2f}, 800 {:.2f}", regular, bold));
    });

    Tests::add("opsz follows size, and an explicit one overrides it", [] {
        auto fonts = Fonts{faces()};
        auto text_cut = style(30);
        text_cut.axes[0] = {tag("opsz"), 14};
        const auto display = fonts.measure("Hamburgefonstiv", style(30)).width; // opsz 30
        const auto text = fonts.measure("Hamburgefonstiv", text_cut).width;
        expect_true(std::abs(display - text) > 0.5, std::format("opsz 30 {:.2f}, opsz 14 {:.2f}", display, text));
    });

    Tests::add("italic picks the italic face", [] {
        auto fonts = Fonts{faces()};
        auto italic = style(30);
        italic.italic = true;
        const auto upright = fonts.measure("Hamburgefonstiv", style(30)).width;
        const auto slanted = fonts.measure("Hamburgefonstiv", italic).width;
        expect_true(std::abs(upright - slanted) > 0.1, std::format("upright {:.2f}, italic {:.2f}", upright, slanted));
    });

    Tests::add("paragraphs wrap, and a line limit ends in an ellipsis", [] {
        auto fonts = Fonts{faces()};
        const auto text = "one two three four five six seven eight nine ten eleven twelve";
        const auto free = fonts.measure(text, style(16), 120);
        expect_true(free.lines > 3 && !free.truncated, std::format("{} lines", free.lines));
        const auto held = fonts.measure(text, style(16), 120, 2);
        expect_true(held.lines == 2 && held.truncated, std::format("{} lines, truncated {}", held.lines, held.truncated));
        expect_true(held.height < free.height, "held is shorter");
    });

    Tests::add("a line too long for its frame is truncated", [] {
        auto fonts = Fonts{faces()};
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(200, 40));
        const auto fits = fonts.line(*surface->getCanvas(), "Short", style(16), {0, 0, 200, 40}, SK_ColorBLACK);
        const auto cut = fonts.line(*surface->getCanvas(), "A sentence far too long for this frame", style(16), {0, 0, 120, 40}, SK_ColorBLACK);
        expect_true(!fits.truncated && cut.truncated, std::format("fits {}, cut {}", fits.truncated, cut.truncated));
        expect_true(cut.width <= 120.01, std::format("width {:.2f}", cut.width));
    });

    Tests::add("axis positions are quantized: nearby weights share an instance", [] {
        auto fonts = Fonts{faces()};
        fonts.measure("x", style(20, 400.1f));
        fonts.measure("x", style(20, 400.2f));
        fonts.measure("x", style(20, 399.9f));
        expect_true(fonts.cached_instances() == 1, std::format("{} instances", fonts.cached_instances()));
    });

    Tests::add("a sweep stays bounded", [] {
        auto fonts = Fonts{faces()};
        for (auto wt = 100; wt <= 900; ++wt) fonts.measure("Sweep", style(20, static_cast<float>(wt)));
        expect_true(fonts.cached_instances() <= 128 && fonts.cached_layouts() <= 256,
                    std::format("{} instances, {} layouts", fonts.cached_instances(), fonts.cached_layouts()));
    });

    Tests::add("tracking sits between letters, none after the last", [] {
        auto fonts = Fonts{faces()};
        auto tracked = style(40);
        tracked.tracking = 100; // 4 px at 40
        const auto one = fonts.measure("H", style(40)).width;
        expect_true(std::abs(fonts.measure("H", tracked).width - one) < 0.01, "one letter: no tracking");
        const auto two = fonts.measure("HH", style(40)).width;
        const auto two_tracked = fonts.measure("HH", tracked).width;
        expect_true(std::abs(two_tracked - two - 4) < 0.01, std::format("HH {:.2f}, tracked {:.2f}", two, two_tracked));
        expect_true(std::abs(fonts.ink("H", tracked).left - fonts.ink("H", style(40)).left) < 0.01, "the first letter doesn't move");
    });

    Tests::add("Anchor::Cap centres capitals on y, Anchor::X lowercase", [] {
        auto fonts = Fonts{faces()};
        const auto cap = drawn(300, 200, [&](SkCanvas& c) { fonts.text(c, "HIH", style(80), {20, 100}, SK_ColorBLACK); });
        expect_true(std::abs(cap.cy() - 100) <= 1, std::format("capitals span {}..{}", cap.top, cap.bottom));
        const auto x = drawn(300, 200, [&](SkCanvas& c) { fonts.text(c, "xxx", style(80), {20, 100}, SK_ColorBLACK, {.anchor = Anchor::X}); });
        expect_true(std::abs(x.cy() - 100) <= 1, std::format("lowercase spans {}..{}", x.top, x.bottom));
        const auto base = drawn(300, 200, [&](SkCanvas& c) { fonts.text(c, "H", style(80), {20, 100}, SK_ColorBLACK, {.anchor = Anchor::Baseline}); });
        expect_true(std::abs(base.bottom + 1 - 100) <= 1, std::format("baseline at {}", base.bottom + 1));
        const auto ink = drawn(300, 200, [&](SkCanvas& c) { fonts.text(c, "(g)", style(80), {20, 100}, SK_ColorBLACK, {.anchor = Anchor::Ink}); });
        expect_true(std::abs(ink.cy() - 100) <= 1, std::format("ink spans {}..{}", ink.top, ink.bottom));
    });

    Tests::add("alignment by advances, and optically by outlines", [] {
        auto fonts = Fonts{faces()};
        const auto right = drawn(400, 200, [&](SkCanvas& c) { fonts.text(c, "T", style(120), {300, 100}, SK_ColorBLACK, {.align = Align::Right, .optical = true}); });
        expect_true(std::abs(right.right + 1 - 300) <= 1, std::format("optical right edge at {}", right.right + 1));
        const auto left = drawn(400, 200, [&](SkCanvas& c) { fonts.text(c, "T", style(120), {100, 100}, SK_ColorBLACK, {.optical = true}); });
        expect_true(std::abs(left.left - 100) <= 1, std::format("optical left edge at {}", left.left));
        const auto by_advance = drawn(400, 200, [&](SkCanvas& c) { fonts.text(c, "T", style(120), {100, 100}, SK_ColorBLACK); });
        expect_true(by_advance.left > left.left, "the advance box leaves a side bearing");
        const auto centred = drawn(400, 200, [&](SkCanvas& c) { fonts.text(c, "HH", style(80), {200, 100}, SK_ColorBLACK, {.align = Align::Center, .optical = true}); });
        expect_true(std::abs((centred.left + centred.right + 1) / 2. - 200) <= 1, std::format("centred {}..{}", centred.left, centred.right));
    });

    Tests::add("a tracked centred label centres its letters, not a trailing space", [] {
        auto fonts = Fonts{faces()};
        auto tracked = style(40);
        tracked.tracking = 300;
        const auto d = drawn(400, 100, [&](SkCanvas& c) { fonts.text(c, "HIH", tracked, {200, 50}, SK_ColorBLACK, {.align = Align::Center}); });
        expect_true(std::abs((d.left + d.right + 1) / 2. - 200) <= 1.5, std::format("letters {}..{}", d.left, d.right));
    });

    Tests::add("font metrics: table and outline heights", [] {
        auto fonts = Fonts{faces()};
        const auto m = fonts.metrics(style(100));
        expect_true(m.ink_cap_height > 60 && m.ink_cap_height < 80, std::format("cap {:.2f}", m.ink_cap_height));
        expect_true(m.ink_x_height > 40 && m.ink_x_height < m.ink_cap_height, std::format("x {:.2f}", m.ink_x_height));
        expect_true(std::abs(m.cap_height - m.ink_cap_height) < 2, std::format("table cap {:.2f}", m.cap_height));
        expect_true(m.ascent > m.ink_cap_height && m.descent > 0 && m.line_height >= m.ascent + m.descent, "ascent, descent, line height");
        auto spaced = style(20);
        spaced.line_height = 1.5f;
        expect_true(std::abs(fonts.metrics(spaced).line_height - 30) < 0.01, "an explicit line height");
        expect_true(std::abs(-fonts.ink("H", style(100)).top - m.ink_cap_height) < 0.01, "ink agrees");
    });

    Tests::add("Case::Upper is full Unicode, and raises punctuation to capitals", [] {
        auto fonts = Fonts{faces()};
        auto upper = style(40);
        upper.transform = Case::Upper;
        expect_true(std::abs(fonts.measure("straße", upper).width - fonts.measure("STRASSE", style(40)).width) < 0.01, "ß becomes SS");
        const auto plain = fonts.ink("(", style(40));
        const auto cased = fonts.ink("(", upper);
        expect_true(cased.top < plain.top - 0.5, std::format("( top {:.2f} -> {:.2f}", plain.top, cased.top));
        auto lower = style(40);
        lower.transform = Case::Lower;
        expect_true(std::abs(fonts.measure("ÉCOLE", lower).width - fonts.measure("école", style(40)).width) < 0.01, "lowercase");
    });

    Tests::add("Overflow: Shrink fits by size, Clip and Visible keep it", [] {
        auto fonts = Fonts{faces()};
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(300, 60));
        auto& c = *surface->getCanvas();
        const auto text = "Preset name that runs long";
        const auto natural = fonts.measure(text, style(20)).width;
        const auto frame = tiny::Frame{0, 0, natural * 0.7, 40};
        const auto shrunk = fonts.line(c, text, style(20), frame, SK_ColorBLACK, {.overflow = Overflow::Shrink});
        expect_true(shrunk.size < 20 && shrunk.size >= 10 && !shrunk.truncated && shrunk.width <= frame.w + 0.01,
                    std::format("size {:.2f}, width {:.2f} in {:.2f}", shrunk.size, shrunk.width, frame.w));
        const auto floor = fonts.line(c, text, style(20), {0, 0, natural * 0.3, 40}, SK_ColorBLACK, {.overflow = Overflow::Shrink, .min_size = 16});
        expect_true(floor.size == 16 && floor.truncated, std::format("at the floor: size {:.2f}, truncated {}", floor.size, floor.truncated));
        const auto clipped = fonts.line(c, text, style(20), frame, SK_ColorBLACK, {.overflow = Overflow::Clip});
        expect_true(clipped.size == 20 && clipped.truncated, "clip keeps the size and reports the overrun");
    });

    Tests::add("spans: one baseline, each its own style and width", [] {
        auto fonts = Fonts{faces()};
        const auto value = Span{"+4.0", style(30, 500)};
        const auto unit = Span{" dB", style(18)};
        const auto spans = std::array{value, unit};
        const auto both = fonts.measure(spans).width;
        const auto apart = fonts.measure("+4.0", style(30, 500)).width + fonts.measure(" dB", style(18)).width;
        expect_true(std::abs(both - apart) < 0.5, std::format("together {:.2f}, apart {:.2f}", both, apart));
        const auto d = drawn(300, 100, [&](SkCanvas& c) {
            const auto coloured = std::array{Span{"+4.0", style(30, 500), SK_ColorBLACK}, Span{" dB", style(18), SK_ColorBLACK}};
            fonts.text(c, coloured, {20, 50}, {.anchor = Anchor::Baseline});
        });
        expect_true(d.bottom + 1 <= 51, std::format("both runs on the baseline: bottom {}", d.bottom + 1));
    });

    if (const auto* path = std::getenv("TINY_TEXT_PNG")) render(path);
    return Tests::run_all() == 0 ? 0 : 1;
}
