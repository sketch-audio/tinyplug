# Plan: Fonts and text — embedded fonts, HarfBuzz shaping, one result on every platform

> Status: **design.** tinyplug draws no real text: `tiny_ui` has a 3×5 pixel font, and products bring their own
> stack (all_plugins' `text_helpers`: a Skia `Font_manager` plus a CoreText / DirectWrite `Typesetter`). This plan
> adds a model-free `tiny_text` library: fonts embedded in the binary, static or variable, shaped with HarfBuzz
> through SkShaper, rasterised by each platform's Skia font manager. Same glyph positions on macOS, iOS, Windows
> and web.

## Why the earlier Skia text looked wrong

Most likely not Skia's quality, but no shaper in the path:

- **`SkFont` + `drawString` / `drawSimpleText` doesn't kern.** It maps characters to glyphs one by one and advances
  by each glyph's width. Kerning (`kern`, GPOS) and ligatures come only from a shaper. Spacing that "looks wrong
  next to CoreText" is unshaped text. `SkShapers::Primitive` has the same limit; only the HarfBuzz shaper kerns.
- **Hinted advances.** With hinting on and linear metrics off, advances round to whole pixels at the font's size,
  so small UI text gets uneven gaps. Wanted: `setSubpixel(true)`, `setLinearMetrics(true)`, hinting `kNone` or
  `kSlight`, edging `kAntiAlias`.
- **HarfBuzz doesn't link today.** tiny_deps builds it (`build/tmp/skia/<arch>/libharfbuzz.a`) but ships only
  `libskshaper.a`, which has 734 unresolved `hb_` symbols. Only the primitive shaper could ever have linked.

HarfBuzz is what Chrome and Android shape with on every platform, Chrome on macOS included. The A/B tool (step 3)
checks the claim against CoreText for our fonts before anything depends on it.

## What exists

| | Where | Notes |
|---|---|---|
| Pixel font | [tiny_ui/draw.hpp](../libs/tiny_ui/include/tiny_ui/draw.hpp) | `text`, `text_in`, `text_width`; demos only |
| `Font_manager` | `all_plugins/shared/text_helpers` | `SkFontMgr_New_CoreText` / `_DirectWrite`, `makeFromData`, string keys; no web |
| `Typesetter` | same | CoreText / DirectWrite glyphs + positions; fonts registered **process-wide** (`CTFontManagerRegisterGraphicsFont`) by a singleton |
| Font data | same | 5,000-line `.h` byte arrays, all included by one header; brand fonts, some commercial (GT Maru) |
| `Painter` | `all_plugins/shared/monorobot` | the newest client text code; uses only `Font_manager::make_font`, measures with `SkFont` |
| Skia modules | tiny_deps, every platform | `skshaper`, `skparagraph`, `skunicode_core` + `libgrapheme` (no ICU), `svg`, built and unused |

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Shaper | **HarfBuzz via SkShaper, every platform** | Kerning and features; identical positions everywhere, web included |
| Rasteriser | The platform `SkFontMgr`: CoreText (Apple), DirectWrite (Windows), FreeType custom-empty (web) | Glyphs render the way the OS renders them. Positions match across platforms; pixels differ by rasteriser, as native apps do |
| Font source | **Embedded data only**, `makeFromData`; nothing registered with the OS | No process-global state shared with other plug-ins or other versions of ours in one host (the ObjC class-name problem again) |
| Native typesetter | Kept as a **test oracle** (step 3), macOS only | A backend only if the A/B shows a gap HarfBuzz can't close |
| Fallback | Declared families in order; then, natively, the system manager's per-character fallback | A user-typed preset name in another script still draws. Web has no system fonts: a product that needs it embeds a fallback |
| Lines | **Single-line runs first** (labels, values, names); multi-line through `skparagraph` later | Almost all plug-in text is one line |
| Variable fonts | **Supported**: one embedded file, axes chosen per `Style`, live changes allowed | One file instead of cut instances (monorobot embeds Google Sans Flex instances); every backend supports it (below) |
| Units | Shaped in **logical** units; the editor's canvas scale does the rest | Shaping doesn't depend on the scale factor, so a cached run survives a move between displays |
| Fonts in the repo | One **OFL** family for `tiny_ui` and the demos, in tiny_deps with its licence | The demos stop drawing pixel text. Products embed their own; commercial fonts never land in tinyplug |

## Shape

```
tiny_deps/
  build-skia-{mac,ios,win,web}.sh   also ship libharfbuzz.a, libgrapheme.a (+ whatever skunicode / skshaper
                                    leave unresolved); setup_skia.cmake imports them
  fonts/<family>/                   the OFL family and its licence
tinyplug/
  cmake/embed.cmake                 tiny_embed(<target> NAME <ns> FILES ...): byte arrays in a generated .cpp
  libs/tiny_text/                   STATIC, model-free; Skia + skshaper + harfbuzz + skunicode PRIVATE
    include/tiny_text/fonts.hpp     font registry
    include/tiny_text/text.hpp      shaping, measuring, drawing a run
    source/
  libs/tiny_ui/                     draw.hpp's text helpers move onto tiny_text; the pixel font goes
  tools/text_ab/                    macOS: CoreText against HarfBuzz, positions and pixels
```

**Embedding** (`tiny_embed`) is general: fonts first, then SVG and images. A `cmake -P` script at build time writes
`alignas(16) const unsigned char[]` arrays and a header of `std::span<const std::byte>` accessors, one `.cpp` per
file so a change rebuilds one object. Replaces all_plugins' string-escaped `embed_file`, which only works for text.
C23 `#embed` later, when MSVC has it.

## API sketch

```cpp
namespace tiny::text {

struct Face {
    std::string_view family{};
    int weight{400};       // a static face's weight; a variable face matches any weight its `wght` axis covers
    bool italic{};
    std::span<const std::byte> data{};
};

struct Axis {
    std::uint32_t tag{};   // OpenType tag: 'wdth', 'slnt', 'GRAD', ...
    double value{};
};

// Owns the typefaces and the shaped-run cache. One per editor; main thread.
class Fonts {
public:
    explicit Fonts(std::span<const Face> faces);

    class Handle; // what the editor draws with
    auto handle() -> Handle;
};

struct Style {
    std::string_view family{};
    double weight{400};    // a variable face's `wght`; a static face picks the nearest weight
    bool italic{};
    double size{12};       // logical; also sets `opsz` on a face that has it
    double tracking{};     // em/1000, as designers specify it
    Features features{};   // tnum, case, ss01, ... (OpenType tags)
    Axes axes{};           // other axes, a small fixed array of `Axis`; ignored by static faces
};

struct Metrics { double width{}, ascent{}, descent{}, cap_height{}, x_height{}; };

class Fonts::Handle {
public:
    auto measure(std::string_view utf8, const Style&) const -> Metrics;
    auto draw(SkCanvas&, std::string_view utf8, const Style&, double x, double baseline, SkColor) const -> void;
    auto draw_in(SkCanvas&, std::string_view utf8, const Style&, const Frame&, Align, SkColor) const -> void;
    auto fit(std::string_view utf8, const Style&, double max_width) const -> std::string; // ellipsis
};

}
```

**Tabular figures** (`tnum`) are the reason `features` is in v1: a value readout in proportional figures jitters
sideways as it changes. Most UI fonts have them; only a shaper applies them.

## Variable fonts

Checked in the m139 source tiny_deps pins: every backend makes a typeface at a variation position
(`onMakeClone(SkFontArguments)` in `SkTypeface_mac_ct.cpp`, `SkTypeface_win_dw.cpp`, `SkFontHost_FreeType.cpp`),
Skia's bundled FreeType defines `TT_CONFIG_OPTION_GX_VAR_SUPPORT`, so web has them too, and
`SkShaper_harfbuzz.cpp` hands the typeface's `getVariationDesignPosition` to HarfBuzz, so advances and kerning
follow the axes, not the default instance.

- **An instance is a typeface.** A `Style` resolves to the face's typeface cloned at its axis position, cached by
  (face, position). Natively it's the same: in CoreText a variation is a distinct `CTFont`, glyphs at a new
  position are new outlines and rasterise again, and a `CTLine` is rebuilt as the advances change.
- **`opsz` is always set explicitly**, from `Style::size` unless `axes` overrides it. CoreText applies optical size
  from the point size on its own, DirectWrite and FreeType don't (Skia's Mac port works around the CoreText
  behaviour, `OpszVariation`); setting it ourselves gives one result everywhere.
- **Live axes** (an animated weight, a hover emphasis) are allowed. Each new position costs a clone, a cold glyph
  cache and a HarfBuzz face; reshaping every frame is microseconds. So positions are **quantized** (an axis step
  per axis, `wght` 1 unit by default: invisible, and a 400→700 sweep is at most 300 instances) and kept in an LRU
  with the shaped runs, so a sweep back and forth reuses warm instances. Large text is drawn from paths by Skia
  and barely touches the glyph cache. Chrome animates `font-variation-settings` the same way, on Skia.
- **Windows needs 10 1709 or later** for DirectWrite variations (`IDWriteFontFace5`). The documented minimum.
- Named instances (`Bold`, `Condensed`) aren't in the API: a `Style` names axis values. A product wanting names
  keeps its own constants.

**The cache.** An immediate-mode editor redraws every frame, so it shapes the same strings every frame. Shaped
runs are cached as `SkTextBlob`s keyed by (face, size, tracking, features, string), a small LRU per `Fonts`,
evicting what a frame didn't touch. Variation instances live beside them, keyed by quantized position. Shaping allocates; this is main-thread editor code, which may.

## Order of work

0. **tiny_deps.** Ship HarfBuzz and the unicode deps for all four platforms; import them in `setup_skia.cmake`. A
   probe that shapes one string with `SkShapers::HB` links on each. Record the size cost, above all the web
   controller's (739-755 KB brotli now).
1. **`tiny_embed`**, with a test that round-trips a binary file.
2. **`tiny_text`**: `Fonts`, shaping, measure, draw, fit, the cache, variation instances. Unit tests: kerning
   applied (`AV` narrower than `A` + `V`), `tnum` applied, identical positions across rasterisers for one face, a
   heavier `wght` measuring wider, `opsz` set on every backend.
3. **The A/B tool**, before migrating anything: our fonts × sizes 9-24 × strings (pairs, numbers, all caps,
   tracking), CoreText (the old `Typesetter`, moved in as a test oracle) against HarfBuzz; diff positions and
   render both. Decides whether an Apple backend is ever needed. Includes a variable face at several `wght` and
   `opsz` positions, against a `CTFont` at the same variation.
3b. **Live-axis probe** in `platform_demo`: a label sweeping `wght` at 60 fps, frame time on macOS (Metal),
   Windows (CPU) and web (CPU). Sets the default quantization step and LRU size.
4. **`tiny_ui` and the demos** on the OFL family; the pixel font goes. Web demos get real text.
5. **Clients** (after [client-port.md](client-port.md)): `Font_manager` and `Typesetter` replaced by `tiny_text`,
   `Painter` ported, font data through `tiny_embed`. Products then build for web with their own fonts.
6. Later: multi-line text through `skparagraph` (tooltips, about pages, licence text); SVG icons through `svg` and
   `tiny_embed`.

## Open questions

- **Which OFL family?** Inter, IBM Plex Sans or Source Sans 3: tabular figures, a medium weight, a compact size.
  Prefer its variable release (Inter and Source Sans 3 have one, with `opsz` in Inter 4), so the demos exercise
  variation.
- **Fonts per editor or per process?** Per editor is simplest and private; typeface creation is a few ms. A
  process-wide registry (still unregistered with the OS) only if several open editors measure badly.
- **LCD (subpixel) antialiasing on Windows?** Off at first: it needs an opaque background under the text and is
  wrong on layers. Revisit if Windows text looks soft at 100 % scale.

## Rejected

- **Native typesetters as the path** (CoreText, DirectWrite): three implementations, none on web, positions that
  differ by platform, and process-wide font registration on Apple.
- **System fonts**: no web equivalent, and a product's look would depend on the user's machine.
- **A shaped-text cache shared with the view loop**: text is editor drawing; the framework's view loop stays
  ignorant of it.
