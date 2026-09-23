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
constexpr auto low_key = 60; // One octave from middle C, top row highest.
constexpr auto num_rows = 12;

constexpr auto note_color = SkColorSetRGB(96, 200, 120);
constexpr auto playhead_color = SkColorSetRGB(230, 190, 80);
constexpr auto strip_colors = std::array{SkColorSetRGB(80, 170, 220), SkColorSetRGB(200, 110, 160)};

struct Theme {
    SkColor background{};
    SkColor panel{};
    SkColor black_row{};
    SkColor grid{};
    SkColor ink{};
    SkColor dim{};
};

auto theme(bool dark) -> Theme
{
    if (dark) return {SkColorSetRGB(22, 22, 26), SkColorSetRGB(32, 32, 38), SkColorSetRGB(27, 27, 32),
                      SkColorSetRGB(58, 58, 66), SkColorSetRGB(210, 210, 220), SkColorSetRGB(80, 80, 90)};
    return {SkColorSetRGB(244, 244, 248), SkColorSetRGB(232, 232, 238), SkColorSetRGB(220, 220, 228),
            SkColorSetRGB(206, 206, 214), SkColorSetRGB(40, 40, 48), SkColorSetRGB(180, 180, 190)};
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
    return std::clamp(static_cast<int>(std::floor((pos.x - grid.x) / grid.w * num_steps)), 0, num_steps - 1);
}

auto key_at(const Frame& grid, Coords pos) -> int
{
    const auto row = std::clamp(static_cast<int>(std::floor((pos.y - grid.y) / grid.h * num_rows)), 0, num_rows - 1);
    return low_key + (num_rows - 1 - row);
}

constexpr auto is_black = std::array{false, true, false, true, false, false, true, false, true, false, true, false};

} // namespace

auto Editor::_layout(Rect_size size) const -> Layout
{
    const auto w = static_cast<double>(size.w);
    const auto h = static_cast<double>(size.h);
    const auto grid_w = w - margin * (2 + num_params) - strip_w * num_params;
    const auto grid_h = h - 3. * margin - button_h;
    const auto buttons_y = margin + grid_h + margin;

    auto layout = Layout{
        .grid = {margin, margin, grid_w, grid_h},
        .undo = {margin, buttons_y, button_w, button_h},
        .redo = {margin * 2. + button_w, buttons_y, button_w, button_h},
    };
    for (auto i = size_t{}; i < num_params; ++i) {
        layout.strips[i] = {margin * 2 + grid_w + static_cast<double>(i) * (strip_w + margin), margin, strip_w, grid_h};
    }
    return layout;
}

// One edit per cell a stroke touches: set that key on the step, or rest it.
auto Editor::_draw(const Frame& grid, Coords pos, bool first) -> void
{
    const auto column = static_cast<size_t>(column_at(grid, pos));
    const auto key = static_cast<std::uint8_t>(key_at(grid, pos));
    if (first) _erasing = (_edit.state.view().key[column] == key);

    _edit.state.edit([column, key, erase = _erasing](models::State& s) {
        if (erase) {
            if (s.key[column] == key) s.key[column] = models::State::rest;
        }
        else {
            s.key[column] = key;
        }
    });
}

auto Editor::_end_stroke() -> void
{
    if (!_drawing) return;
    _drawing = false;
    _edit.state.commit(); // One undo step per stroke.
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
    _end_stroke();
    _end_drag();
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view_context = state.view_context;
    auto* canvas = view_context.canvas;
    if (!canvas) return;

    const auto layout = _layout(view_context.logical_size);

    for (const auto& event : view_context.interaction.events.events) {
        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                if (down.button != Pointer_button::left) return;
                if (layout.grid.contains(down.pos)) {
                    _drawing = true;
                    _draw(layout.grid, down.pos, true);
                    return;
                }
                for (auto i = size_t{}; i < num_params; ++i) {
                    if (!layout.strips[i].contains(down.pos)) continue;
                    _dragging = static_cast<int>(i);
                    _edit.actions.push(Action_start{static_cast<uint32_t>(i)});
                    _set(_dragging, layout.strips[i], down.pos);
                    return;
                }
                if (layout.undo.contains(down.pos)) _edit.undo_redo.undo();
                else if (layout.redo.contains(down.pos)) _edit.undo_redo.redo();
            },
            [&](const Pointer_move& move) {
                if (_drawing && layout.grid.contains(move.pos)) _draw(layout.grid, move.pos, false);
                if (_dragging >= 0) _set(_dragging, layout.strips[static_cast<size_t>(_dragging)], move.pos);
            },
            [&](const Pointer_up&) {
                _end_stroke();
                _end_drag();
            },
            [&](const Pointer_cancel&) {
                _end_stroke();
                _end_drag();
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

    // Rows shaded like a keyboard, a line every beat, notes as cells, the playing step outlined.
    const auto grid = rect(layout.grid);
    fill(*canvas, grid, colors.panel);
    const auto col_w = grid.width() / static_cast<float>(num_steps);
    const auto row_h = grid.height() / static_cast<float>(num_rows);
    for (auto row = 0; row < num_rows; ++row) {
        const auto key = low_key + (num_rows - 1 - row);
        if (is_black[static_cast<size_t>(key % 12)]) {
            fill(*canvas, SkRect::MakeXYWH(grid.left(), grid.top() + row_h * static_cast<float>(row), grid.width(), row_h), colors.black_row);
        }
    }
    for (auto c = 0; c < num_steps; ++c) {
        const auto x = grid.left() + col_w * static_cast<float>(c);
        if (c % 4 == 0) fill(*canvas, SkRect::MakeXYWH(x, grid.top(), 1.f, grid.height()), colors.grid);

        const auto key = pattern.key[static_cast<size_t>(c)];
        if (key != models::State::rest && key >= low_key && key < low_key + num_rows) {
            const auto row = num_rows - 1 - (key - low_key);
            fill(*canvas, SkRect::MakeXYWH(x + 2.f, grid.top() + row_h * static_cast<float>(row) + 2.f, col_w - 4.f, row_h - 4.f), note_color);
        }

        if (c == playing) {
            auto paint = SkPaint{};
            paint.setColor(playhead_color);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(2.f);
            canvas->drawRect(SkRect::MakeXYWH(x + 1.f, grid.top() + 1.f, col_w - 2.f, grid.height() - 2.f), paint);
        }
    }

    // Undo / redo, dimmed when there is nothing to walk to.
    fill(*canvas, rect(layout.undo), colors.panel);
    fill(*canvas, rect(layout.redo), colors.panel);
    arrow(*canvas, layout.undo, true, _edit.undo_redo.can_undo() ? colors.ink : colors.dim);
    arrow(*canvas, layout.redo, false, _edit.undo_redo.can_redo() ? colors.ink : colors.dim);

    // Gate, velocity.
    for (auto i = size_t{}; i < num_params; ++i) {
        const auto strip = rect(layout.strips[i]);
        fill(*canvas, strip, colors.panel);
        const auto h = static_cast<float>(state.processor_state.params[i]) * strip.height();
        fill(*canvas, SkRect::MakeXYWH(strip.left(), strip.bottom() - h, strip.width(), h), strip_colors[i]);
    }

    canvas->restore();
}

} // namespace tiny::edit
