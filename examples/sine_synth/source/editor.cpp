#include "editor.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace tiny::edit {

namespace {

constexpr auto margin = 12.;
constexpr auto strips_h = 110.;
constexpr auto first_key = 60;             // Middle C.
constexpr auto num_white = 14;             // Two octaves.
constexpr auto black_w = 0.6;              // Of a white key.
constexpr auto black_h = 0.62;             // Of the keyboard.
constexpr auto slide_semis = 2.;           // Tuning per white-key width slid sideways.

constexpr auto lit_color = SkColorSetRGB(96, 200, 120);
constexpr auto strip_colors = std::array{
    SkColorSetRGB(230, 190, 80), SkColorSetRGB(220, 140, 80), SkColorSetRGB(200, 110, 160),
    SkColorSetRGB(130, 120, 220), SkColorSetRGB(80, 170, 220), SkColorSetRGB(96, 200, 120),
};

struct Theme {
    SkColor background{};
    SkColor panel{};
    SkColor white_key{};
    SkColor black_key{};
    SkColor edge{};
};

auto theme(bool dark) -> Theme
{
    if (dark) return {SkColorSetRGB(22, 22, 26), SkColorSetRGB(32, 32, 38), SkColorSetRGB(210, 210, 220),
                      SkColorSetRGB(40, 40, 48), SkColorSetRGB(22, 22, 26)};
    return {SkColorSetRGB(244, 244, 248), SkColorSetRGB(232, 232, 238), SkColorSetRGB(252, 252, 255),
            SkColorSetRGB(40, 40, 48), SkColorSetRGB(180, 180, 190)};
}

auto rect(const Frame& f) -> SkRect
{
    return SkRect::MakeXYWH(static_cast<float>(f.x), static_cast<float>(f.y), static_cast<float>(f.w), static_cast<float>(f.h));
}

auto fill(SkCanvas& canvas, SkRect r, SkColor color) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setAntiAlias(true);
    canvas.drawRect(r, paint);
}

auto mix(SkColor a, SkColor b, float t) -> SkColor
{
    const auto lerp = [t](U8CPU x, U8CPU y) { return static_cast<U8CPU>(static_cast<float>(x) + t * (static_cast<float>(y) - static_cast<float>(x))); };
    return SkColorSetRGB(lerp(SkColorGetR(a), SkColorGetR(b)), lerp(SkColorGetG(a), SkColorGetG(b)), lerp(SkColorGetB(a), SkColorGetB(b)));
}

// The white-key index within an octave each semitone sits on or after, and whether it is black.
constexpr auto white_of = std::array{0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
constexpr auto is_black = std::array{false, true, false, true, false, false, true, false, true, false, true, false};

auto white_w(const Frame& keys) -> double { return keys.w / num_white; }

auto key_frame(const Frame& keys, int key) -> Frame
{
    const auto semi = (key - first_key) % 12;
    const auto octave = (key - first_key) / 12;
    const auto ww = white_w(keys);
    const auto x = keys.x + (octave * 7 + white_of[static_cast<size_t>(semi)]) * ww;
    if (is_black[static_cast<size_t>(semi)]) {
        return {x + ww - ww * black_w / 2, keys.y, ww * black_w, keys.h * black_h};
    }
    return {x, keys.y, ww, keys.h};
}

// Black keys sit on top, so they win.
auto key_at(const Frame& keys, Coords pos) -> std::optional<int>
{
    if (!keys.contains(pos)) return std::nullopt;
    for (auto key = first_key; key < first_key + num_white / 7 * 12; ++key) {
        if (is_black[static_cast<size_t>((key - first_key) % 12)] && key_frame(keys, key).contains(pos)) return key;
    }
    for (auto key = first_key; key < first_key + num_white / 7 * 12; ++key) {
        if (!is_black[static_cast<size_t>((key - first_key) % 12)] && key_frame(keys, key).contains(pos)) return key;
    }
    return std::nullopt;
}

} // namespace

auto Editor::_layout(Rect_size size) const -> Layout
{
    const auto w = static_cast<double>(size.w);
    const auto h = static_cast<double>(size.h);
    const auto strip_w = (w - margin * (num_params + 1)) / num_params;

    auto layout = Layout{};
    for (auto i = size_t{}; i < num_params; ++i) {
        layout.strips[i] = {margin + static_cast<double>(i) * (strip_w + margin), margin, strip_w, strips_h};
    }
    layout.keys = {margin, margin * 2 + strips_h, w - margin * 2, h - margin * 3 - strips_h};
    return layout;
}

auto Editor::_press(uintptr_t tag, const Frame& keys, Coords pos) -> void
{
    const auto key = key_at(keys, pos);
    if (!key) return;

    auto* free = std::find_if(_touches.begin(), _touches.end(), [](const Touch& t) { return !t.used; });
    if (free == _touches.end()) return;

    // Low on the key is loud, as on a real one.
    const auto frame = key_frame(keys, *key);
    const auto velocity = 0.3 + 0.7 * std::clamp((pos.y - frame.y) / frame.h, 0., 1.);

    *free = Touch{.tag = tag, .id = _next_id++, .key = static_cast<uint8_t>(*key), .down = pos, .used = true};
    if (_next_id == 0) _next_id = 1; // 0 means "no id".
    _edit.notes.send(midi::Note::On{{free->id, 0, free->key}, static_cast<float>(velocity)});
}

// Up for pressure, sideways for tuning. Sent once per frame per finger, below.
auto Editor::_slide(uintptr_t tag, const Frame& keys, Coords pos) -> void
{
    for (auto& touch : _touches) {
        if (!touch.used || touch.tag != tag) continue;
        touch.pressure = std::clamp((touch.down.y - pos.y) / (keys.h * 0.5), 0., 1.);
        touch.tuning = std::clamp((pos.x - touch.down.x) / white_w(keys) * slide_semis, -12., 12.);
        touch.moved = true;
    }
}

auto Editor::_lift(uintptr_t tag) -> void
{
    for (auto& touch : _touches) {
        if (!touch.used || touch.tag != tag) continue;
        touch.used = false;
        const auto off = midi::Note::Off{{touch.id, 0, touch.key}, 0.f};
        if (!_edit.notes.send(off)) _unsent.push_back(off);
    }
}

auto Editor::_set(int strip, const Frame& frame, Coords pos) -> void
{
    const auto knob = std::clamp((frame.y + frame.h - pos.y) / frame.h, 0., 1.);
    _edit.actions.push(Set_param{static_cast<uint32_t>(strip), knob});
}

auto Editor::_end_drag() -> void
{
    if (_dragging < 0) return;
    _edit.actions.push(Action_end{static_cast<uint32_t>(_dragging)});
    _dragging = -1;
}

auto Editor::notify(const Host_event& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Editor::on_gui_hide() -> void
{
    // A key held while the window closes would sound forever: release everything.
    for (auto& touch : _touches) {
        if (touch.used) _lift(touch.tag);
    }
    _end_drag();
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view_context = state.view_context;
    auto* canvas = view_context.canvas;
    if (!canvas) return;

    const auto layout = _layout(view_context.logical_size);

    // Offs the pipe refused last time go first.
    std::erase_if(_unsent, [&](const midi::Note::Off& off) { return _edit.notes.send(off); });

    for (const auto& event : view_context.interaction.events.events) {
        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                if (down.button != Pointer_button::left) return;
                for (auto i = size_t{}; i < num_params; ++i) {
                    if (!layout.strips[i].contains(down.pos)) continue;
                    _dragging = static_cast<int>(i);
                    _edit.actions.push(Action_start{static_cast<uint32_t>(i)});
                    _set(_dragging, layout.strips[i], down.pos);
                    return;
                }
                _press(event.pointer_tag, layout.keys, down.pos);
            },
            [&](const Pointer_move& move) {
                if (_dragging >= 0) _set(_dragging, layout.strips[static_cast<size_t>(_dragging)], move.pos);
                _slide(event.pointer_tag, layout.keys, move.pos);
            },
            [&](const Pointer_up&) {
                _end_drag();
                _lift(event.pointer_tag);
            },
            [&](const Pointer_cancel&) {
                _end_drag();
                _lift(event.pointer_tag);
            },
            [](const auto&) {}
        }, event.event);
    }

    // At most one pressure and one tuning per finger per frame: the pipe is a queue, not a mailbox.
    for (auto& touch : _touches) {
        if (!touch.used || !touch.moved) continue;
        using Kind = midi::Note::Expression::Kind;
        const auto note = midi::Note::Id{touch.id, 0, touch.key};
        _edit.notes.send(midi::Note::Expression{note, Kind::Pressure, touch.pressure});
        _edit.notes.send(midi::Note::Expression{note, Kind::Tuning, touch.tuning});
        touch.moved = false;
    }

    const auto colors = theme(_dark);
    const auto& lit = state.processor_state.blocks.latest<models::Blocks::Address::Keys>().level;

    canvas->save();
    canvas->scale(static_cast<float>(view_context.scale), static_cast<float>(view_context.scale));
    fill(*canvas, SkRect::MakeWH(static_cast<float>(view_context.logical_size.w), static_cast<float>(view_context.logical_size.h)), colors.background);

    // Parameters: attack, decay, sustain, release, vibrato, level.
    for (auto i = size_t{}; i < num_params; ++i) {
        const auto strip = rect(layout.strips[i]);
        fill(*canvas, strip, colors.panel);
        const auto h = static_cast<float>(state.processor_state.params[i]) * strip.height();
        fill(*canvas, SkRect::MakeXYWH(strip.left(), strip.bottom() - h, strip.width(), h), strip_colors[i]);
    }

    // Keys, white under black. A lit key brightens with its pressure.
    const auto draw_key = [&](int key, bool black) {
        const auto r = rect(key_frame(layout.keys, key));
        const auto level = lit[static_cast<size_t>(key)];
        const auto base = black ? colors.black_key : colors.white_key;
        const auto color = level == 0 ? base : mix(mix(base, lit_color, 0.6f), SkColorSetRGB(255, 255, 255), static_cast<float>(level - 1) / 254.f * 0.5f);
        fill(*canvas, r, color);
        auto edge = SkPaint{};
        edge.setColor(colors.edge);
        edge.setStyle(SkPaint::kStroke_Style);
        edge.setStrokeWidth(1.f);
        canvas->drawRect(r, edge);
    };
    for (auto key = first_key; key < first_key + 24; ++key) {
        if (!is_black[static_cast<size_t>((key - first_key) % 12)]) draw_key(key, false);
    }
    for (auto key = first_key; key < first_key + 24; ++key) {
        if (is_black[static_cast<size_t>((key - first_key) % 12)]) draw_key(key, true);
    }

    canvas->restore();
}

} // namespace tiny::edit
