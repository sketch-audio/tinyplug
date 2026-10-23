#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <tiny_core/tiny_input.hpp> // Coords, Frame

class SkCanvas;

namespace tiny::text {

// Text for editors: embedded fonts, static or variable, shaped with HarfBuzz and laid out by SkParagraph, drawn by
// the platform's rasteriser. Logical units throughout; scale the canvas by the view's scale first. Main thread.

// An OpenType tag: tag("wdth"), tag("tnum").
constexpr auto tag(const char (&s)[5]) -> std::uint32_t
{
    return (std::uint32_t(std::uint8_t(s[0])) << 24) | (std::uint32_t(std::uint8_t(s[1])) << 16)
         | (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}

// A font file under a family name. The bytes must outlive the `Fonts` (embedded data does).
struct Face {
    std::string_view family{};
    std::span<const std::byte> data{};
};

struct Axis {
    std::uint32_t tag{}; // 0 ends the list
    float value{};
};

struct Feature {
    std::uint32_t tag{}; // 0 ends the list
    int value{1};
};

// A case transform, applied before shaping (full Unicode, through libgrapheme). `Upper` also turns on the font's
// case-sensitive forms (`case`): punctuation, dashes and brackets sit on capitals instead of on lowercase.
enum class Case { Normal, Upper, Lower };

struct Style {
    std::string_view family{};
    float size{12};
    float weight{400};               // a variable face's `wght`; a static family picks its nearest weight
    bool italic{};
    float tracking{};                // em/1000, between letters: none after the last on a line
    float line_height{};             // a multiple of `size`; 0 keeps the font's own
    Case transform{Case::Normal};
    bool hinted{true};               // false: unhinted outlines, as a browser on macOS draws them
    std::array<Feature, 4> features{};
    std::array<Axis, 4> axes{};      // `opsz` follows `size` unless set here
};

// A run of one style in a line or paragraph: a value and its unit, a bold term in a sentence.
struct Span {
    std::string_view utf8{};
    Style style{};
    std::uint32_t color{0xff000000}; // An SkColor.
};

enum class Align { Left, Center, Right };

// The horizontal line a single line of text is placed on: the `y` given, or a frame's middle.
enum class Anchor {
    Cap,      // capitals centred on it: how labels are placed
    X,        // lowercase centred on it
    Ink,      // this text's own outlines centred on it: figures, symbols, mixed case
    Box,      // the font's ascent-to-descent box centred on it
    Baseline, // the baseline on it; in a frame, on the frame's bottom edge
};

// What a single line in a frame does when it's wider than the frame.
enum class Overflow {
    Ellipsis, // cut at the last character that fits, and end in an ellipsis
    Shrink,   // drawn smaller, down to `Place::min_size`, then cut with an ellipsis
    Clip,     // cut at the frame's edges
    Visible,  // drawn whole, past the frame
};

struct Place {
    Align align{Align::Left};
    Anchor anchor{Anchor::Cap};
    Overflow overflow{Overflow::Ellipsis}; // in a frame only
    float min_size{};                      // Shrink's floor; 0 is half the style's size
    bool optical{};                        // align on the outlines' edges, not the advances: no side bearings at the edges
};

struct Metrics {
    double width{};    // the longest line, tracking after its last letter excluded
    double height{};
    double baseline{}; // the first line's, from the top
    std::size_t lines{};
    bool truncated{};  // lines or width ran out and an ellipsis was drawn
    float size{};      // the size drawn: below the style's when Overflow::Shrink shrank it
};

// A font at a style: distances from the baseline, all positive.
struct Font_metrics {
    double ascent{};
    double descent{};
    double leading{};
    double line_height{};    // the distance between baselines in a paragraph of this style
    double cap_height{};     // from the font's tables
    double x_height{};
    double ink_cap_height{}; // from the outlines of H and x: what a designer measures
    double ink_x_height{};
};

// Outline bounds, from the text's origin on its baseline (y down: `top` is negative above the baseline).
struct Ink {
    double left{}, top{}, right{}, bottom{};
    auto width() const -> double { return right - left; }
    auto height() const -> double { return bottom - top; }
};

// Owns the typefaces and the laid-out text, for one editor.
class Fonts {
public:

    explicit Fonts(std::span<const Face> faces);
    ~Fonts();

    Fonts(const Fonts&) = delete;
    auto operator=(const Fonts&) -> Fonts& = delete;

    // One line at a point: `at.x` is the edge (or centre) `place.align` names, `at.y` the line `place.anchor` names.
    auto text(SkCanvas& canvas, std::string_view utf8, const Style& style, Coords at, std::uint32_t color, Place place = {}) -> Metrics;
    auto text(SkCanvas& canvas, std::span<const Span> spans, Coords at, Place place = {}) -> Metrics;

    // One line in a frame: aligned across it, anchored on its middle, `place.overflow` when it doesn't fit.
    auto line(SkCanvas& canvas, std::string_view utf8, const Style& style, const Frame& frame, std::uint32_t color, Place place = {}) -> Metrics;
    auto line(SkCanvas& canvas, std::span<const Span> spans, const Frame& frame, Place place = {}) -> Metrics;

    // Wrapped to `frame.w` from the top of `frame`; `max_lines` 0 is unlimited, otherwise the last ends in an ellipsis.
    auto paragraph(SkCanvas& canvas, std::string_view utf8, const Style& style, const Frame& frame, std::uint32_t color, Align align = Align::Left, std::size_t max_lines = 0) -> Metrics;
    auto paragraph(SkCanvas& canvas, std::span<const Span> spans, const Frame& frame, Align align = Align::Left, std::size_t max_lines = 0) -> Metrics;

    // One line, unbounded.
    auto measure(std::string_view utf8, const Style& style) -> Metrics;
    auto measure(std::span<const Span> spans) -> Metrics;

    // Wrapped to `width`.
    auto measure(std::string_view utf8, const Style& style, double width, std::size_t max_lines = 0) -> Metrics;

    // One line's outline bounds, and the font's metrics at a style (the instance its axes name).
    auto ink(std::string_view utf8, const Style& style) -> Ink;
    auto metrics(const Style& style) -> Font_metrics;

    // Typeface instances and laid-out runs held now: what the cache costs while axes move.
    auto cached_instances() const -> std::size_t;
    auto cached_layouts() const -> std::size_t;

private:

    struct Impl;
    std::unique_ptr<Impl> _impl;

};

} // namespace tiny::text
