#include "editor.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"

#include <algorithm>
#include <cmath>

namespace tiny::edit {

namespace {

constexpr auto margin = 12.;
constexpr auto strip_w = 28.;
constexpr auto button_h = 32.;
constexpr auto button_w = 64.;
constexpr auto num_steps = static_cast<int>(models::State::num_steps);

constexpr auto bar_color = SkColorSetRGB(96, 200, 120);
constexpr auto playhead_color = SkColorSetRGB(230, 190, 80);

struct Theme {
    SkColor background{};
    SkColor panel{};
    SkColor grid{};
    SkColor ink{};
    SkColor dim{};
};

auto theme(bool dark) -> Theme
{
    if (dark) return {SkColorSetRGB(22, 22, 26), SkColorSetRGB(32, 32, 38), SkColorSetRGB(58, 58, 66),
                      SkColorSetRGB(210, 210, 220), SkColorSetRGB(80, 80, 90)};
    return {SkColorSetRGB(244, 244, 248), SkColorSetRGB(232, 232, 238), SkColorSetRGB(206, 206, 214),
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

// A triangle pointing left (undo) or right (redo) inside `f`.
auto arrow(SkCanvas& canvas, const Frame& f, bool left, SkColor color) -> void
{
    const auto cx = static_cast<float>(f.x + f.w / 2);
    const auto cy = static_cast<float>(f.y + f.h / 2);
    const auto s = static_cast<float>(f.h * 0.25);
    const auto dir = left ? -1.f : 1.f;

    auto path = SkPath{};
    path.moveTo(cx + dir * s, cy);
    path.lineTo(cx - dir * s, cy - s);
    path.lineTo(cx - dir * s, cy + s);
    path.close();

    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setAntiAlias(true);
    canvas.drawPath(path, paint);
}

auto column_at(const Frame& grid, Coords pos) -> int
{
    const auto t = (pos.x - grid.x) / grid.w;
    return std::clamp(static_cast<int>(std::floor(t * num_steps)), 0, num_steps - 1);
}

auto level_at(const Frame& grid, Coords pos) -> int
{
    const auto t = (grid.y + grid.h - pos.y) / grid.h;
    return std::clamp(static_cast<int>(std::lround(t * 100.)), 0, 100);
}

} // namespace

auto Editor::_layout(Rect_size size) const -> Layout
{
    const auto w = static_cast<double>(size.w);
    const auto h = static_cast<double>(size.h);
    const auto grid_w = w - 3. * margin - strip_w;
    const auto grid_h = h - 3. * margin - button_h;
    const auto buttons_y = margin + grid_h + margin;

    return {
        .grid = {margin, margin, grid_w, grid_h},
        .undo = {margin, buttons_y, button_w, button_h},
        .redo = {margin * 2. + button_w, buttons_y, button_w, button_h},
        .depth = {w - margin - strip_w, margin, strip_w, grid_h},
    };
}

// Every touched column in one edit, interpolating across columns a fast move skipped.
auto Editor::_paint(const Frame& grid, Coords pos, bool first) -> void
{
    const auto column = column_at(grid, pos);
    const auto level = level_at(grid, pos);
    const auto c0 = first ? column : _last_column;
    const auto l0 = first ? level : _last_level;
    _last_column = column;
    _last_level = level;

    _edit.state.edit([c0, l0, c1 = column, l1 = level](models::State& s) {
        const auto lo = std::min(c0, c1);
        const auto hi = std::max(c0, c1);
        for (auto c = lo; c <= hi; ++c) {
            const auto t = (c0 == c1) ? 1. : static_cast<double>(c - c0) / static_cast<double>(c1 - c0);
            const auto l = std::lround(static_cast<double>(l0) + t * static_cast<double>(l1 - l0));
            s.level[static_cast<size_t>(c)] = static_cast<std::uint8_t>(l);
        }
    });
}

auto Editor::_end_stroke() -> void
{
    if (!_painting) return;
    _painting = false;
    _edit.state.commit(); // One undo step per stroke.
}

auto Editor::_set_depth(const Frame& strip, Coords pos) -> void
{
    _depth = std::clamp((strip.y + strip.h - pos.y) / strip.h, 0., 1.);
    _edit.actions.push(Set_param{enum_raw(Address::Depth), _depth});
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
    _end_stroke(); // A stroke cut off by the window closing still becomes a step.
    if (_dragging_depth) {
        _edit.actions.push(Action_end{enum_raw(Address::Depth)});
        _dragging_depth = false;
    }
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view_context = state.view_context;
    auto* canvas = view_context.canvas;
    if (!canvas) return;

    const auto layout = _layout(view_context.logical_size);
    const auto depth_addr = enum_raw(Address::Depth);
    _depth = state.processor_state.params[depth_addr];

    // Input first, so this frame draws what it just did.
    for (const auto& event : view_context.interaction.events.events) {
        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                if (down.button != Pointer_button::left) return;
                if (layout.grid.contains(down.pos)) {
                    _painting = true;
                    _paint(layout.grid, down.pos, true);
                }
                else if (layout.depth.contains(down.pos)) {
                    _dragging_depth = true;
                    _edit.actions.push(Action_start{depth_addr});
                    _set_depth(layout.depth, down.pos);
                }
                else if (layout.undo.contains(down.pos)) {
                    _edit.undo_redo.undo();
                }
                else if (layout.redo.contains(down.pos)) {
                    _edit.undo_redo.redo();
                }
            },
            [&](const Pointer_move& move) {
                if (_painting) _paint(layout.grid, move.pos, false);
                if (_dragging_depth) _set_depth(layout.depth, move.pos);
            },
            [&](const Pointer_up& up) {
                if (_painting) _paint(layout.grid, up.pos, false);
                _end_stroke();
                if (_dragging_depth) {
                    _set_depth(layout.depth, up.pos);
                    _edit.actions.push(Action_end{depth_addr});
                    _dragging_depth = false;
                }
            },
            [&](const Pointer_cancel&) {
                _end_stroke();
                if (_dragging_depth) {
                    _edit.actions.push(Action_end{depth_addr});
                    _dragging_depth = false;
                }
            },
            [](const auto&) {}
        }, event.event);
    }

    const auto colors = theme(_dark);
    const auto& pattern = _edit.state.view();
    const auto playing = static_cast<int>(std::lround(state.processor_state.meters[enum_raw(Meter::Step)]));

    canvas->save();
    canvas->scale(static_cast<float>(view_context.scale), static_cast<float>(view_context.scale));
    fill(*canvas, SkRect::MakeWH(static_cast<float>(view_context.logical_size.w), static_cast<float>(view_context.logical_size.h)), colors.background);

    // Pattern: one bar per sixteenth, the playing step outlined.
    const auto grid = rect(layout.grid);
    fill(*canvas, grid, colors.panel);
    const auto col_w = grid.width() / static_cast<float>(num_steps);
    for (auto c = 0; c < num_steps; ++c) {
        const auto x = grid.left() + col_w * static_cast<float>(c);
        if (c % 4 == 0) fill(*canvas, SkRect::MakeXYWH(x, grid.top(), 1.f, grid.height()), colors.grid);

        const auto level = static_cast<float>(pattern.level[static_cast<size_t>(c)]) / 100.f;
        const auto bar_h = level * grid.height();
        fill(*canvas, SkRect::MakeXYWH(x + 3.f, grid.bottom() - bar_h, col_w - 6.f, bar_h), bar_color);

        if (c == playing) {
            auto paint = SkPaint{};
            paint.setColor(playhead_color);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(2.f);
            canvas->drawRect(SkRect::MakeXYWH(x + 1.f, grid.top() + 1.f, col_w - 2.f, grid.height() - 2.f), paint);
        }
    }

    // Undo / redo, dimmed when there is nothing to walk to.
    const auto can_undo = _edit.undo_redo.can_undo();
    const auto can_redo = _edit.undo_redo.can_redo();
    fill(*canvas, rect(layout.undo), colors.panel);
    fill(*canvas, rect(layout.redo), colors.panel);
    arrow(*canvas, layout.undo, true, can_undo ? colors.ink : colors.dim);
    arrow(*canvas, layout.redo, false, can_redo ? colors.ink : colors.dim);

    // Depth.
    const auto strip = rect(layout.depth);
    fill(*canvas, strip, colors.panel);
    const auto depth_h = static_cast<float>(_depth) * strip.height();
    fill(*canvas, SkRect::MakeXYWH(strip.left(), strip.bottom() - depth_h, strip.width(), depth_h), colors.ink);

    canvas->restore();
}

} // namespace tiny::edit
