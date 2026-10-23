# Plan: Fonts and text — embedded fonts, HarfBuzz shaping, one result on every platform

> Status: **landed for the framework (2026-10-23); the clients remain.** `tiny_text`
> ([fonts.hpp](../libs/tiny_text/include/tiny_text/fonts.hpp)) and `tiny_embed` ([embed.cmake](../cmake/embed.cmake))
> are in, with [text_demo](../examples/text_demo/) (Inter, variable) and `tiny_text_test` (18 tests, native and wasm).
> Built and checked by eye on macOS (VST3, AU, CLAP in a DAW), Windows, iOS (a device, in an AUv3 host) and web
> (headless Chrome). What remains is the client port ([client-port.md](client-port.md), "Text") and the items under
> "Next". Text fields that type in the same fonts are [keyboard-input.md](keyboard-input.md)'s `platform::Text`.
>
> Fonts embedded in the binary, static or variable, shaped with HarfBuzz and laid out by SkParagraph, rasterised by
> each platform's Skia font manager. Same glyph positions on every platform. `tiny_ui` and the other demos keep the
> 3×5 pixel font.

## Why the earlier Skia text looked wrong

Most likely not Skia's quality, but no shaper in the path:

- **`SkFont` + `drawString` / `drawSimpleText` doesn't kern.** It maps characters to glyphs one by one and advances
  by each glyph's width. Kerning (`kern`, GPOS) and ligatures come only from a shaper; `SkShapers::Primitive` has
  the same limit. Only HarfBuzz kerns.
- **Hinted advances.** With hinting on and subpixel positioning off, advances round to whole pixels at the font's
  size. SkParagraph sets subpixel, slight hinting and antialiased edging.
- **SkParagraph's rounding hack** (`ParagraphStyle::setApplyRoundingHack`, on by default, a Flutter legacy) rounds
  widths. `tiny_text` turns it off.

HarfBuzz is already in tiny_deps: the official build compiles it into `libskshaper.a`, and libgrapheme plus the bidi
code into `libskunicode_libgrapheme.a` (no ICU tables, kept small on purpose). Read a universal archive one slice
at a time (`nm -arch arm64`) or its cross-object references look unresolved.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Engine | **SkParagraph** for single lines and paragraphs alike: HarfBuzz shaping, line breaking, ellipsis, letter spacing, OpenType features, variation arguments | One engine; single lines are paragraphs held to one line |
| Unicode | libgrapheme (`SkUnicodes::Libgrapheme`), no ICU | Binary size; also what a Linux build would use |
| Rasteriser | The platform `SkFontMgr`: CoreText (Apple), DirectWrite (Windows, loaded at runtime by Skia), FreeType custom-empty (web) | Glyphs render the way the OS renders them. Positions match across platforms; pixels differ by rasteriser |
| Font source | **Embedded data only**, `makeFromData` into a `TypefaceFontProvider`; nothing registered with the OS | No process-global state shared with other plug-ins or other versions of ours in one host |
| Fallback | Declared families; then, natively, the platform manager's per-character fallback | A user-typed name in another script still draws. Web has no system fonts |
| Variable fonts | One file per family, `wght` from `Style::weight`, `opsz` from `Style::size`, other axes in `Style::axes`, live changes allowed | Below |
| Colour | Not part of a layout: a painter supplies it at draw time | A hover colour change never costs a layout |
| Units | Logical; the editor's canvas scale does the rest | A cached layout survives a move between displays |
| Ownership | **One `Fonts` per editor**, main thread | Simple and private; typeface creation is cheap |
| Demo font | **Inter 4.1** (SIL OFL 1.1), variable, upright and italic, in `examples/text_demo/fonts` with its licence | `wght` 100-900 and `opsz` 14-32; only text_demo uses it |

## Shape

```
tiny_deps/cmake/setup_skia.cmake      tiny::skparagraph (-> skshaper -> skunicode, skia)
tinyplug/
  cmake/embed.cmake, embed_file.cmake tiny_embed(<target> NAME <name> FILES ...)
  libs/tiny_text/                     STATIC, model-free; Skia PRIVATE (the header forward-declares SkCanvas)
  examples/text_demo/                 single line and multiline, live weight, frame rate and cache counts
  tests/tiny_text_test.cpp            TINY_TEXT_PNG=<path> also renders a specimen
```

**`tiny_embed`** writes, at build time, one `.cpp` per file (an `alignas(16)` array) and `<embedded/<name>.hpp>`
declaring `embedded::<name>::<file>() -> std::span<const std::byte>`. ~1 s for an 880 KB font. For fonts now, SVG
and images later. C23 `#embed` when MSVC has it.

**API** (`tiny::text`, [fonts.hpp](../libs/tiny_text/include/tiny_text/fonts.hpp)):

- `Style{family, size, weight, italic, tracking, line_height, transform, hinted, features, axes}`; `Span{utf8,
  style, color}` for runs of several styles on one baseline or in one paragraph.
- `Fonts::text` (one line at a point), `Fonts::line` (one line in a frame), `Fonts::paragraph` (wrapped, optional
  line limit), `Fonts::measure`, each for a string or spans, returning `Metrics{width, height, baseline, lines,
  truncated, size}`.
- `Place{align, anchor, overflow, min_size, optical}` positions a single line. `Anchor`: `Cap` (default: capitals
  centred on the line, as labels are placed), `X`, `Ink`, `Box`, `Baseline`. `Overflow` in a frame: `Ellipsis`
  (character-level), `Shrink` (down to `min_size`, then ellipsis), `Clip` (across only), `Visible`. `optical`
  aligns on the outlines' edges instead of the advances, so no side bearing shows at a margin.
- `Fonts::metrics(style)`: ascent, descent, leading, line height, the tables' cap and x height, and the `H` and
  `x` outline heights (what a designer measures). `Fonts::ink(utf8, style)`: a line's outline bounds.
- **Tracking is a designer's**: em/1000 between letters, none after the last, so a tracked label centres on its
  letters. SkParagraph adds spacing after every glyph and shifts a left line by half a space; single lines are
  laid out left-aligned and placed by tiny_text, which takes both out.
- **The painted baseline**: SkParagraph rounds a line's baseline to a whole logical pixel; placement works from
  the rounded one, so an anchor lands where it says (pixel-tested to 1 px).
- `Case::Upper` / `Lower` transform with full Unicode (libgrapheme's, already linked: `ß` → `SS`); `Upper` also
  turns on `case`, the font's case-sensitive forms. `hinted = false` draws unhinted outlines.
- `tag("tnum")` builds OpenType tags.

**Tabular figures** are why `features` is in v1: a readout in proportional figures shifts sideways as it changes.

## Variable fonts

Every backend in the m139 source makes a typeface at a variation position (`onMakeClone(SkFontArguments)` in the
CoreText, DirectWrite and FreeType ports; Skia's FreeType defines `TT_CONFIG_OPTION_GX_VAR_SUPPORT`).
SkParagraph clones through `FontCollection::findTypefaces(…, fontArgs)`, and the HarfBuzz shaper reads the clone's
position, so advances and kerning follow the axes.

- **An instance is a typeface**, as natively: a CoreText variation is its own `CTFont`, glyphs at a new position
  rasterise again, a `CTLine` is rebuilt.
- **`opsz` is set explicitly** from `Style::size` unless `axes` overrides it. CoreText applies optical size on its
  own, the other ports don't.
- **Live axes**: positions are quantized (`wght` to 1 unit, others to ¼) so a sweep reuses instances. The
  collection's typeface cache is dropped past 128 distinct positions (built layouts keep theirs) and layouts are an
  LRU of 256, so a sweep stays bounded (tested). text_demo's editor redraws its whole page each frame at 59 fps in
  headless Chrome with the sweep off; the sweep's frame time is unmeasured (text_demo's header shows it live).
- **Windows needs 10 1709 or later** for DirectWrite variations.

## Measured (2026-10-23)

| Web controller, brotli | GainDemo | TextDemo |
|---|---|---|
| total | 751 KB | 1,959 KB |
| of which Inter, upright + italic | — | 693 KB |
| text engine (SkParagraph, HarfBuzz, FreeType, libgrapheme) | — | ~515 KB |

A product embedding one weight-limited static font pays much less than the two full variable files.

## Next

1. ~~Windows and iOS builds~~: both run text_demo (2026-10-23).
2. ~~The CoreText A/B tool~~: dropped. text_demo's spacing reads natural by eye in a DAW; build one (all_plugins'
   `Typesetter` as the oracle, diffing positions) only if a product's font shows a difference.
3. **Paragraph tracking**: a tracked paragraph's lines still carry SkParagraph's trailing space and half-space shift
   (single lines don't); a paragraph's last-line ellipsis breaks at a word (`drawn …`). Fix if a product needs it.
4. **Clients** (after [client-port.md](client-port.md)): monorobot's `Painter` and SUI's `Text` onto `tiny_text`,
   font data through `tiny_embed`, then `text_helpers` goes. Products then build for web with their own fonts.
5. Later: SVG icons through Skia's `svg` module and `tiny_embed`; LCD antialiasing on Windows only if text looks soft
   at 100 %.

**Linux** ([linux-feasibility.md](linux-feasibility.md)) needs nothing new from this design: the FreeType backend web
already uses (`makeFromData`, no system fonts required), HarfBuzz in `skshaper` and libgrapheme build there as
everywhere. The one choice is the fallback manager for characters our fonts lack: fontconfig (the user's fonts) or
none (as web).

## Rejected

- **Native typesetters as the path** (CoreText, DirectWrite): three implementations, none on web, positions that
  differ by platform, and process-wide font registration on Apple.
- **System fonts**: no web equivalent, and a product's look would depend on the user's machine.
- **SkShaper + `SkTextBlob` directly for single lines**: SkParagraph already does it, plus ellipsis, alignment and
  wrapping, with one cache.
- **Replacing the pixel font**: the demos keep it; it's part of their look.
