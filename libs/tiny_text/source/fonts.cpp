#include "tiny_text/fonts.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tiny_core/platform_defs.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skunicode/include/SkUnicode_libgrapheme.h"

extern "C" {
#include "third_party/externals/libgrapheme/grapheme.h" // Case transforms; already linked into skunicode.
}

#if TINY_PLATFORM_APPLE
#include "include/ports/SkFontMgr_mac_ct.h"
#elif TINY_PLATFORM_WINDOWS
#include "include/ports/SkTypeface_win.h"
#elif TINY_PLATFORM_WASM
#include "include/ports/SkFontMgr_empty.h"
#endif

namespace tiny::text {

namespace tl = skia::textlayout;

namespace {

// The platform's manager: it makes our typefaces (so the OS rasterises them) and is the per-character fallback.
auto platform_font_mgr() -> sk_sp<SkFontMgr>
{
#if TINY_PLATFORM_APPLE
    return SkFontMgr_New_CoreText(nullptr);
#elif TINY_PLATFORM_WINDOWS
    return SkFontMgr_New_DirectWrite();
#elif TINY_PLATFORM_WASM
    return SkFontMgr_New_Custom_Empty(); // No system fonts: fallback finds nothing.
#endif
}

// Axis positions and sizes are quantized so a live sweep reuses instances: a quarter unit is far below anything visible.
auto quantize(float v) -> float { return std::round(v * 4.f) / 4.f; }

constexpr auto ellipsis = "\xE2\x80\xA6";
constexpr auto max_layouts = std::size_t{256};
constexpr auto max_instances = std::size_t{128}; // Distinct axis positions before the collection's typefaces are dropped.
constexpr auto unbounded = 1e6f;

auto to_align(Align align) -> tl::TextAlign
{
    switch (align) {
        case Align::Left: return tl::TextAlign::kLeft;
        case Align::Center: return tl::TextAlign::kCenter;
        case Align::Right: return tl::TextAlign::kRight;
    }
    return tl::TextAlign::kLeft;
}

auto tag_string(std::uint32_t t) -> SkString
{
    const char s[4] = {char(t >> 24), char(t >> 16), char(t >> 8), char(t)};
    return SkString(s, 4);
}

template<typename T>
auto append(std::string& key, const T& value) -> void
{
    key.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

auto transformed(std::string_view s, Case c, std::string& out) -> std::string_view
{
    if (c == Case::Normal || s.empty()) return s;
    const auto convert = c == Case::Upper ? grapheme_to_uppercase_utf8 : grapheme_to_lowercase_utf8;
    const auto n = convert(s.data(), s.size(), nullptr, 0);
    out.resize(n + 1);
    convert(s.data(), s.size(), out.data(), out.size());
    out.resize(n);
    return out;
}

auto letter_spacing(const Style& style) -> float { return quantize(style.size) * style.tracking / 1000.f; }

// Draws every glyph in one colour, so a colour change never costs a layout; or, for spans, in each run's own.
class Painter final : public tl::ParagraphPainter {
public:
    Painter(SkCanvas& canvas, std::optional<SkColor> tint) : _canvas{canvas}, _tint{tint}
    {
        _paint.setColor(tint.value_or(SK_ColorBLACK));
        _paint.setAntiAlias(true);
    }

    void drawTextBlob(const sk_sp<SkTextBlob>& blob, SkScalar x, SkScalar y, const SkPaintOrID& paint) override
    {
        const auto* own = std::get_if<SkPaint>(&paint);
        _canvas.drawTextBlob(blob, x, y, _tint || !own ? _paint : *own);
    }
    void drawTextShadow(const sk_sp<SkTextBlob>&, SkScalar, SkScalar, SkColor, SkScalar) override {}
    void drawRect(const SkRect& rect, const SkPaintOrID& paint) override
    {
        if (const auto* p = std::get_if<SkPaint>(&paint)) _canvas.drawRect(rect, *p);
    }
    void drawFilledRect(const SkRect& rect, const DecorationStyle& style) override { _canvas.drawRect(rect, style.skPaint()); }
    void drawPath(const SkPath& path, const DecorationStyle& style) override { _canvas.drawPath(path, style.skPaint()); }
    void drawLine(SkScalar x0, SkScalar y0, SkScalar x1, SkScalar y1, const DecorationStyle& style) override { _canvas.drawLine(x0, y0, x1, y1, style.skPaint()); }
    void clipRect(const SkRect& rect) override { _canvas.clipRect(rect); }
    void translate(SkScalar dx, SkScalar dy) override { _canvas.translate(dx, dy); }
    void save() override { _canvas.save(); }
    void restore() override { _canvas.restore(); }

private:
    SkCanvas& _canvas;
    std::optional<SkColor> _tint{};
    SkPaint _paint{};
};

auto outline_top(const SkFont& font, SkUnichar c) -> double
{
    auto path = SkPath{};
    if (!font.getPath(font.unicharToGlyph(c), &path)) return 0;
    return -path.getBounds().fTop;
}

auto font_metrics(const SkFont& font, const Style& style) -> Font_metrics
{
    auto m = SkFontMetrics{};
    font.getMetrics(&m);
    auto fm = Font_metrics{
        .ascent = -m.fAscent,
        .descent = m.fDescent,
        .leading = m.fLeading,
        .cap_height = m.fCapHeight,
        .x_height = m.fXHeight,
        .ink_cap_height = outline_top(font, 'H'),
        .ink_x_height = outline_top(font, 'x'),
    };
    fm.line_height = style.line_height > 0 ? quantize(style.size) * style.line_height : fm.ascent + fm.descent + fm.leading;
    return fm;
}

} // namespace

struct Fonts::Impl {
    // A laid-out paragraph, and what placing its first line needs.
    struct Layout {
        std::unique_ptr<tl::Paragraph> paragraph{};
        Font_metrics font{};       // the first run's font
        double left{};             // where the first line's first glyph starts, from the paint origin
        double baseline{};         // the first line's baseline as painted (SkParagraph rounds it), from the top
        double width{};            // the first line, tracking after its last letter excluded
        double trailing{};         // that tracking
        std::optional<Ink> ink{};  // the first line's outlines, from its origin; on first use
        bool tinted{};
        std::uint64_t used{};
    };

    sk_sp<SkUnicode> unicode{SkUnicodes::Libgrapheme::Make()};
    sk_sp<tl::FontCollection> collection{sk_make_sp<tl::FontCollection>()};
    std::unordered_map<std::string, Layout> layouts{};
    std::unordered_set<std::string> instances{};
    std::uint64_t clock{};
    std::vector<std::string> case_buffers{};

    auto text_style(const Style& style, std::optional<SkColor> color, std::string& position) -> tl::TextStyle
    {
        const auto weight = std::round(style.weight);
        const auto size = quantize(style.size);

        auto coords = std::vector<SkFontArguments::VariationPosition::Coordinate>{};
        coords.push_back({SkSetFourByteTag('w', 'g', 'h', 't'), weight});
        const auto opsz = SkSetFourByteTag('o', 'p', 's', 'z');
        if (!std::ranges::any_of(style.axes, [&](const Axis& a) { return a.tag == opsz; })) coords.push_back({opsz, size}); // CoreText sets optical size on its own; the others don't.
        for (const auto& axis : style.axes) {
            if (axis.tag == 0) break;
            coords.push_back({axis.tag, quantize(axis.value)});
        }
        position.assign(style.family);
        append(position, style.italic);
        for (const auto& c : coords) append(position, c);

        auto ts = tl::TextStyle{};
        ts.setFontFamilies({SkString(style.family.data(), style.family.size())});
        ts.setFontSize(size);
        ts.setFontStyle(SkFontStyle(static_cast<int>(weight), SkFontStyle::kNormal_Width,
                                    style.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant));
        ts.setLetterSpacing(letter_spacing(style));
        if (style.line_height > 0) {
            ts.setHeight(style.line_height);
            ts.setHeightOverride(true);
        }
        auto has_case = false;
        for (const auto& feature : style.features) {
            if (feature.tag == 0) break;
            has_case |= feature.tag == tag("case");
            ts.addFontFeature(tag_string(feature.tag), feature.value);
        }
        if (style.transform == Case::Upper && !has_case) ts.addFontFeature(SkString("case"), 1);
        auto args = SkFontArguments{};
        args.setVariationDesignPosition({coords.data(), static_cast<int>(coords.size())});
        ts.setFontArguments(args);
        ts.setColor(color.value_or(SK_ColorBLACK));
        return ts;
    }

    // The layout for these spans at this width, alignment and line limit: cached, or built and laid out now.
    // `tinted`: one colour, supplied at draw time, so the spans' colours aren't part of it.
    auto get(std::span<const Span> spans, float width, Align align, std::size_t max_lines, bool tinted) -> Layout&
    {
        auto key = std::string{};
        append(key, quantize(width));
        append(key, align);
        append(key, max_lines);
        append(key, tinted);
        case_buffers.resize(std::max(case_buffers.size(), spans.size()));
        auto texts = std::vector<std::string_view>{};
        texts.reserve(spans.size());
        auto positions = std::vector<std::string>(spans.size());
        auto styles = std::vector<tl::TextStyle>{};
        styles.reserve(spans.size());
        for (auto i = std::size_t{}; i < spans.size(); ++i) {
            const auto& s = spans[i];
            texts.push_back(transformed(s.utf8, s.style.transform, case_buffers[i]));
            styles.push_back(text_style(s.style, tinted ? std::nullopt : std::optional<SkColor>{s.color}, positions[i]));
            append(key, quantize(s.style.size));
            append(key, s.style.tracking);
            append(key, s.style.line_height);
            append(key, s.style.transform);
            append(key, s.style.hinted);
            for (const auto& feature : s.style.features) {
                if (feature.tag == 0) break;
                append(key, feature);
            }
            if (!tinted) append(key, s.color);
            key += positions[i];
            append(key, texts.back().size());
            key.append(texts.back());
        }

        if (auto found = layouts.find(key); found != layouts.end()) {
            found->second.used = ++clock;
            return found->second;
        }

        for (const auto& position : positions) {
            if (instances.insert(position).second && instances.size() > max_instances) {
                collection->clearCaches(); // Built paragraphs keep their typefaces; only lookups start cold.
                instances.clear();
                instances.insert(position);
            }
        }
        if (layouts.size() >= max_layouts) evict();

        auto paragraph_style = tl::ParagraphStyle{};
        paragraph_style.setTextStyle(styles.front());
        paragraph_style.setTextAlign(to_align(align));
        paragraph_style.setApplyRoundingHack(false); // Flutter's: rounds widths, which shows as uneven spacing.
        if (!spans.front().style.hinted) paragraph_style.turnHintingOff();
        if (max_lines > 0) {
            paragraph_style.setMaxLines(max_lines);
            paragraph_style.setEllipsis(SkString(ellipsis));
        }

        auto builder = tl::ParagraphBuilder::make(paragraph_style, collection, unicode);
        for (auto i = std::size_t{}; i < spans.size(); ++i) {
            builder->pushStyle(styles[i]);
            builder->addText(texts[i].data(), texts[i].size());
            builder->pop();
        }
        auto paragraph = builder->Build();
        paragraph->layout(width);

        auto layout = Layout{.tinted = tinted, .used = ++clock};
        auto lines = std::vector<tl::LineMetrics>{};
        paragraph->getLineMetrics(lines);
        const auto has_text = std::ranges::any_of(texts, [](auto t) { return !t.empty(); });
        if (has_text) layout.font = font_metrics(paragraph->getFontAt(0), spans.front().style);
        if (!lines.empty()) {
            layout.trailing = letter_spacing(spans.back().style);
            layout.left = lines.front().fLeft;
            // As TextLine paints it: the line's top, then its ascent rounded (the run's corrected ascent, which already
            // holds the font's leading and any line height). LineMetrics::fBaseline is Flutter's, from a line height
            // SkParagraph rounds, so rounding it misplaced some sizes by a unit (9 pt Google Sans Flex drew one low).
            const auto& m = lines.front();
            const auto top = m.fBaseline - m.fHeight + m.fDescent;
            layout.baseline = top + std::floor(m.fAscent + 0.5);
            layout.width = std::max(0., lines.front().fWidth - layout.trailing);
        }
        layout.paragraph = std::move(paragraph);
        auto [it, _] = layouts.insert_or_assign(std::move(key), std::move(layout));
        return it->second;
    }

    // Drops the least recently used quarter.
    auto evict() -> void
    {
        auto stamps = std::vector<std::uint64_t>{};
        stamps.reserve(layouts.size());
        for (const auto& [_, layout] : layouts) stamps.push_back(layout.used);
        const auto cut = stamps.begin() + static_cast<std::ptrdiff_t>(stamps.size() / 4);
        std::nth_element(stamps.begin(), cut, stamps.end());
        std::erase_if(layouts, [threshold = *cut](const auto& entry) { return entry.second.used < threshold; });
    }

    static auto ink_of(Layout& layout) -> const Ink&
    {
        if (!layout.ink) {
            auto path = SkPath{};
            layout.paragraph->getPath(0, &path);
            const auto b = path.getBounds();
            layout.ink = path.isEmpty() ? Ink{} : Ink{b.fLeft - layout.left, b.fTop - layout.baseline, b.fRight - layout.left, b.fBottom - layout.baseline};
        }
        return *layout.ink;
    }

    static auto metrics_of(const Layout& layout, float size) -> Metrics
    {
        auto& p = *layout.paragraph;
        return {
            .width = p.lineNumber() <= 1 ? layout.width : std::max(0., p.getLongestLine() - layout.trailing),
            .height = p.getHeight(),
            .baseline = layout.baseline,
            .lines = p.lineNumber(),
            .truncated = p.didExceedMaxLines(),
            .size = size,
        };
    }

    // The baseline that puts `layout`'s anchor on `y`.
    static auto baseline_for(Layout& layout, Anchor anchor, double y) -> double
    {
        const auto& f = layout.font;
        switch (anchor) {
            case Anchor::Cap: return y + f.ink_cap_height / 2;
            case Anchor::X: return y + f.ink_x_height / 2;
            case Anchor::Ink: {
                const auto& ink = ink_of(layout);
                return y - (ink.top + ink.bottom) / 2;
            }
            case Anchor::Box: return y + (f.ascent - f.descent) / 2;
            case Anchor::Baseline: return y;
        }
        return y;
    }

    // Where the first glyph's origin goes so `align` lands on `x`: by the advances, or `optical`ly by the outlines.
    static auto origin_for(Layout& layout, Align align, double x, bool optical) -> double
    {
        auto lo = 0., hi = layout.width;
        if (optical) {
            const auto& ink = ink_of(layout);
            lo = ink.left;
            hi = ink.right;
        }
        switch (align) {
            case Align::Left: return x - lo;
            case Align::Center: return x - (lo + hi) / 2;
            case Align::Right: return x - hi;
        }
        return x;
    }

    auto paint(SkCanvas& canvas, Layout& layout, double origin_x, double baseline_y, std::optional<SkColor> tint) -> void
    {
        auto painter = Painter{canvas, tint};
        layout.paragraph->paint(&painter, static_cast<SkScalar>(origin_x - layout.left), static_cast<SkScalar>(baseline_y - layout.baseline));
    }

    auto place(SkCanvas& canvas, std::span<const Span> spans, Coords at, Place place, bool tinted) -> Metrics
    {
        auto& layout = get(spans, unbounded, Align::Left, 1, tinted);
        const auto baseline = baseline_for(layout, place.anchor, at.y);
        paint(canvas, layout, origin_for(layout, place.align, at.x, place.optical), baseline,
              tinted ? std::optional<SkColor>{spans.front().color} : std::nullopt);
        return metrics_of(layout, quantize(spans.front().style.size));
    }

    auto place_in(SkCanvas& canvas, std::span<const Span> spans, const Frame& frame, Place place, bool tinted) -> Metrics
    {
        // Shrink: scale every span to fit, down to the floor; past it the ellipsis takes over.
        auto scaled = std::vector<Span>(spans.begin(), spans.end());
        auto scale = 1.f;
        if (place.overflow == Overflow::Shrink) {
            const auto natural = get(spans, unbounded, Align::Left, 1, tinted).width;
            if (natural > frame.w && natural > 0) {
                const auto base = spans.front().style.size;
                const auto floor = place.min_size > 0 ? place.min_size : base / 2;
                scale = std::max(floor / base, static_cast<float>(frame.w / natural));
                for (auto& s : scaled) s.style.size *= scale;
                // Optical size and hinting don't scale linearly: one correction pass.
                const auto again = get(scaled, unbounded, Align::Left, 1, tinted).width;
                if (again > frame.w && scale > floor / base) {
                    scale = std::max(floor / base, scale * static_cast<float>(frame.w / again));
                    for (auto i = std::size_t{}; i < scaled.size(); ++i) scaled[i].style.size = spans[i].style.size * scale;
                }
            }
        }

        const auto cut = place.overflow == Overflow::Ellipsis || place.overflow == Overflow::Shrink;
        // The line may run one letter space past the frame: it's after the last letter, so it's never drawn.
        const auto width = cut ? static_cast<float>(frame.w) + letter_spacing(scaled.back().style) : unbounded;
        auto& layout = get(scaled, width, Align::Left, 1, tinted);
        auto metrics = metrics_of(layout, quantize(scaled.front().style.size));
        metrics.truncated = layout.paragraph->didExceedMaxLines() || layout.width > frame.w + 0.01; // Clip and Visible: it overran.

        const auto x = place.align == Align::Left ? frame.x : place.align == Align::Center ? frame.x + frame.w / 2 : frame.x + frame.w;
        const auto y = place.anchor == Anchor::Baseline ? frame.y + frame.h : frame.y + frame.h / 2;
        const auto clip = place.overflow == Overflow::Clip;
        if (clip) {
            canvas.save();
            // Across only: overflow is about width, and a descender below a tight frame is still the text's.
            canvas.clipRect(SkRect::MakeLTRB(static_cast<float>(frame.x), -unbounded, static_cast<float>(frame.x + frame.w), unbounded));
        }
        paint(canvas, layout, origin_for(layout, place.align, x, place.optical), baseline_for(layout, place.anchor, y),
              tinted ? std::optional<SkColor>{scaled.front().color} : std::nullopt);
        if (clip) canvas.restore();
        return metrics;
    }

    auto wrap(SkCanvas& canvas, std::span<const Span> spans, const Frame& frame, Align align, std::size_t max_lines, bool tinted) -> Metrics
    {
        auto& layout = get(spans, static_cast<float>(frame.w), align, max_lines, tinted);
        auto painter = Painter{canvas, tinted ? std::optional<SkColor>{spans.front().color} : std::nullopt};
        layout.paragraph->paint(&painter, static_cast<SkScalar>(frame.x), static_cast<SkScalar>(frame.y));
        return metrics_of(layout, quantize(spans.front().style.size));
    }
};

Fonts::Fonts(std::span<const Face> faces) : _impl{std::make_unique<Impl>()}
{
    const auto mgr = platform_font_mgr();
    auto provider = sk_make_sp<tl::TypefaceFontProvider>();
    for (const auto& face : faces) {
        auto data = SkData::MakeWithoutCopy(face.data.data(), face.data.size());
        if (auto typeface = mgr->makeFromData(std::move(data))) {
            provider->registerTypeface(std::move(typeface), SkString(face.family.data(), face.family.size()));
        }
    }
    _impl->collection->setAssetFontManager(std::move(provider));
    _impl->collection->setDefaultFontManager(mgr);
}

Fonts::~Fonts() = default;

auto Fonts::text(SkCanvas& canvas, std::string_view utf8, const Style& style, Coords at, std::uint32_t color, Place place) -> Metrics
{
    if (utf8.empty()) return {};
    const auto span = Span{utf8, style, color};
    return _impl->place(canvas, {&span, 1}, at, place, true);
}

auto Fonts::text(SkCanvas& canvas, std::span<const Span> spans, Coords at, Place place) -> Metrics
{
    if (spans.empty()) return {};
    return _impl->place(canvas, spans, at, place, false);
}

auto Fonts::line(SkCanvas& canvas, std::string_view utf8, const Style& style, const Frame& frame, std::uint32_t color, Place place) -> Metrics
{
    if (utf8.empty()) return {};
    const auto span = Span{utf8, style, color};
    return _impl->place_in(canvas, {&span, 1}, frame, place, true);
}

auto Fonts::line(SkCanvas& canvas, std::span<const Span> spans, const Frame& frame, Place place) -> Metrics
{
    if (spans.empty()) return {};
    return _impl->place_in(canvas, spans, frame, place, false);
}

auto Fonts::paragraph(SkCanvas& canvas, std::string_view utf8, const Style& style, const Frame& frame, std::uint32_t color, Align align, std::size_t max_lines) -> Metrics
{
    if (utf8.empty()) return {};
    const auto span = Span{utf8, style, color};
    return _impl->wrap(canvas, {&span, 1}, frame, align, max_lines, true);
}

auto Fonts::paragraph(SkCanvas& canvas, std::span<const Span> spans, const Frame& frame, Align align, std::size_t max_lines) -> Metrics
{
    if (spans.empty()) return {};
    return _impl->wrap(canvas, spans, frame, align, max_lines, false);
}

auto Fonts::measure(std::string_view utf8, const Style& style) -> Metrics
{
    if (utf8.empty()) return {};
    const auto span = Span{utf8, style};
    return Impl::metrics_of(_impl->get({&span, 1}, unbounded, Align::Left, 1, true), quantize(style.size));
}

auto Fonts::measure(std::span<const Span> spans) -> Metrics
{
    if (spans.empty()) return {};
    return Impl::metrics_of(_impl->get(spans, unbounded, Align::Left, 1, true), quantize(spans.front().style.size));
}

auto Fonts::measure(std::string_view utf8, const Style& style, double width, std::size_t max_lines) -> Metrics
{
    if (utf8.empty()) return {};
    const auto span = Span{utf8, style};
    return Impl::metrics_of(_impl->get({&span, 1}, static_cast<float>(width), Align::Left, max_lines, true), quantize(style.size));
}

auto Fonts::ink(std::string_view utf8, const Style& style) -> Ink
{
    if (utf8.empty()) return {};
    const auto span = Span{utf8, style};
    return Impl::ink_of(_impl->get({&span, 1}, unbounded, Align::Left, 1, true));
}

auto Fonts::metrics(const Style& style) -> Font_metrics
{
    const auto span = Span{"H", style};
    return _impl->get({&span, 1}, unbounded, Align::Left, 1, true).font;
}

auto Fonts::cached_instances() const -> std::size_t { return _impl->instances.size(); }
auto Fonts::cached_layouts() const -> std::size_t { return _impl->layouts.size(); }

} // namespace tiny::text
