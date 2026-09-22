#include "editor.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace tiny::edit {

namespace {

constexpr auto margin = 12.f;
constexpr auto strip_w = 28.f;
constexpr auto label_h = 16.f;
constexpr auto floor_db = -96.f;
constexpr auto fall_db_per_s = 48.f;
constexpr auto spectrum_color = SkColorSetRGB(96, 200, 120);
constexpr auto scope_color = SkColorSetRGB(90, 150, 235);
constexpr auto free_run_color = SkColorSetRGB(230, 190, 80);

struct Theme {
    SkColor background{};
    SkColor panel{};
    SkColor grid{};
    SkColor text{};
};

auto theme(bool dark) -> Theme
{
    if (dark) return {SkColorSetRGB(22, 22, 26), SkColorSetRGB(32, 32, 38), SkColorSetRGB(58, 58, 66), SkColorSetRGB(170, 170, 180)};
    return {SkColorSetRGB(244, 244, 248), SkColorSetRGB(232, 232, 238), SkColorSetRGB(206, 206, 214), SkColorSetRGB(90, 90, 100)};
}

auto fill(SkCanvas& canvas, SkRect rect, SkColor color) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setAntiAlias(true);
    canvas.drawRect(rect, paint);
}

auto line(SkCanvas& canvas, float x0, float y0, float x1, float y1, SkColor color) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setStrokeWidth(1.f);
    canvas.drawLine(x0, y0, x1, y1, paint);
}

auto stroke(SkCanvas& canvas, const SkPath& path, SkColor color) -> void
{
    auto paint = SkPaint{};
    paint.setColor(color);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1.5f);
    paint.setAntiAlias(true);
    canvas.drawPath(path, paint);
}

// Digits as 3x5 blocks; the examples have no font.
constexpr auto digits = std::array<std::array<uint8_t, 5>, 10>{{
    {0b111, 0b101, 0b101, 0b101, 0b111}, {0b010, 0b110, 0b010, 0b010, 0b111},
    {0b111, 0b001, 0b111, 0b100, 0b111}, {0b111, 0b001, 0b111, 0b001, 0b111},
    {0b101, 0b101, 0b111, 0b001, 0b001}, {0b111, 0b100, 0b111, 0b001, 0b111},
    {0b111, 0b100, 0b111, 0b101, 0b111}, {0b111, 0b001, 0b001, 0b001, 0b001},
    {0b111, 0b101, 0b111, 0b101, 0b111}, {0b111, 0b101, 0b111, 0b001, 0b111},
}};

auto draw_number(SkCanvas& canvas, float x, float y, int value, SkColor color) -> void
{
    constexpr auto px = 2.f;
    auto pen = x;
    for (const auto ch : std::to_string(std::max(value, 0))) {
        const auto& rows = digits[static_cast<size_t>(ch - '0')];
        for (auto row = 0; row < 5; ++row) {
            for (auto col = 0; col < 3; ++col) {
                if ((rows[static_cast<size_t>(row)] >> (2 - col)) & 1) {
                    fill(canvas, SkRect::MakeXYWH(pen + static_cast<float>(col) * px, y + static_cast<float>(row) * px, px, px), color);
                }
            }
        }
        pen += px * 4.f;
    }
}

auto freq_to_x(float hz, float nyquist, SkRect r) -> float
{
    const auto t = std::log(hz / 20.f) / std::log(nyquist / 20.f);
    return r.left() + std::clamp(t, 0.f, 1.f) * r.width();
}

auto db_to_y(float db, SkRect r) -> float
{
    const auto t = std::clamp(db / floor_db, 0.f, 1.f);
    return r.top() + t * r.height();
}

} // namespace

auto Editor::Rate::tick(bool arrived, double dt) -> void
{
    if (arrived) ++count;
    elapsed += dt;
    if (elapsed >= 1.) {
        shown = count;
        count = 0;
        elapsed = 0.;
    }
}

auto Editor::on_gui_show() -> void
{
    const auto addr = enum_raw(Address::Gain);

    auto knob_after = [this](const Drag_info& info) {
        const auto translation = Coords{info.tpos.x - info.fpos.x, info.tpos.y - info.fpos.y};
        const auto dy = (translation.y - _translation.y) / _gain_frame.h;
        _translation = translation;
        return std::clamp(_gain - dy, 0., 1.);
    };

    _drag = std::make_unique<Drag_recognizer>(Gesture_callbacks<Drag_info>{
        .on_started = [=, this](const Drag_info& info) {
            _translation = {};
            _edit.actions.push(Action_start{addr});
            _edit.actions.push(Set_param{addr, knob_after(info)});
        },
        .on_updated = [=, this](const Drag_info& info) {
            _edit.actions.push(Set_param{addr, knob_after(info)});
        },
        .on_ended = [=, this](const Drag_info& info) {
            _edit.actions.push(Set_param{addr, knob_after(info)});
            _edit.actions.push(Action_end{addr});
        },
        .on_cancelled = [this]() { _translation = {}; }
    });
    _drag->set_frame(_gain_frame);
}

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

    const auto now = view_context.time_now;
    const auto dt = _has_time ? Durations::delta_secs(_last_time, now) : 0.;
    _last_time = now;
    _has_time = true;

    const auto w = static_cast<float>(view_context.logical_size.w);
    const auto h = static_cast<float>(view_context.logical_size.h);
    const auto panel_w = (w - 4.f * margin - strip_w) / 2.f;
    const auto panel_h = h - 2.f * margin - label_h;
    const auto spectrum_rect = SkRect::MakeXYWH(margin, margin, panel_w, panel_h);
    const auto scope_rect = SkRect::MakeXYWH(2.f * margin + panel_w, margin, panel_w, panel_h);
    const auto strip_rect = SkRect::MakeXYWH(w - margin - strip_w, margin, strip_w, panel_h);

    // Gain strip: update the gesture before drawing so this frame reflects it.
    const auto gain_frame = Frame{strip_rect.x(), strip_rect.y(), strip_rect.width(), strip_rect.height()};
    if (_gain_frame != gain_frame) {
        _gain_frame = gain_frame;
        if (_drag) _drag->set_frame(_gain_frame);
    }
    _gain = state.processor_state.params[enum_raw(Address::Gain)];
    if (_drag) _drag->process_events(view_context.interaction.events);

    const auto& frames = state.processor_state.blocks;
    const auto& spectrum = frames.latest<Block::Spectrum>();
    const auto* sweep = frames.fresh<Block::Scope>();
    _spectrum_rate.tick(frames.fresh<Block::Spectrum>() != nullptr, dt);
    _scope_rate.tick(sweep != nullptr, dt);
    if (sweep) _scope = *sweep;

    // Peaks fall at a fixed rate and are pushed back up by whatever `latest` holds.
    const auto fall = static_cast<float>(fall_db_per_s * dt);
    for (auto k = size_t{}; k < _display.size(); ++k) {
        const auto incoming = k < spectrum.used ? spectrum.db[k] : floor_db;
        _display[k] = std::max(std::max(_display[k] - fall, floor_db), incoming);
    }

    const auto colors = theme(_dark);
    canvas->save();
    canvas->scale(static_cast<float>(view_context.scale), static_cast<float>(view_context.scale));
    fill(*canvas, SkRect::MakeWH(w, h), colors.background);

    // Spectrum: log frequency, 0 to -96 dBFS.
    fill(*canvas, spectrum_rect, colors.panel);
    const auto nyquist = spectrum.sample_rate > 0.f ? spectrum.sample_rate / 2.f : 24000.f;
    for (const auto hz : {100.f, 1000.f, 10000.f}) {
        const auto x = freq_to_x(hz, nyquist, spectrum_rect);
        line(*canvas, x, spectrum_rect.top(), x, spectrum_rect.bottom(), colors.grid);
    }
    for (auto db = -24.f; db > floor_db; db -= 24.f) {
        const auto y = db_to_y(db, spectrum_rect);
        line(*canvas, spectrum_rect.left(), y, spectrum_rect.right(), y, colors.grid);
    }
    if (spectrum.used > 1 && spectrum.fft_size > 0) {
        const auto bin_hz = spectrum.sample_rate / static_cast<float>(spectrum.fft_size);
        auto path = SkPath{};
        for (auto k = size_t{1}; k < spectrum.used; ++k) {
            const auto x = freq_to_x(static_cast<float>(k) * bin_hz, nyquist, spectrum_rect);
            const auto y = db_to_y(_display[k], spectrum_rect);
            if (k == 1) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        canvas->save();
        canvas->clipRect(spectrum_rect);
        stroke(*canvas, path, spectrum_color);
        canvas->restore();
    }

    // Scope: -1..1, centre line.
    fill(*canvas, scope_rect, colors.panel);
    line(*canvas, scope_rect.left(), scope_rect.centerY(), scope_rect.right(), scope_rect.centerY(), colors.grid);
    if (_scope.used > 1) {
        auto path = SkPath{};
        for (auto i = size_t{}; i < _scope.used; ++i) {
            const auto x = scope_rect.left() + scope_rect.width() * static_cast<float>(i) / static_cast<float>(_scope.used - 1);
            const auto y = scope_rect.centerY() - std::clamp(_scope.samples[i], -1.f, 1.f) * scope_rect.height() * 0.5f;
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        canvas->save();
        canvas->clipRect(scope_rect);
        stroke(*canvas, path, _scope.triggered ? scope_color : free_run_color);
        canvas->restore();
    }

    // Arrivals per second under each panel: the transport's real delivery rate in this host.
    const auto label_y = spectrum_rect.bottom() + 5.f;
    draw_number(*canvas, spectrum_rect.left(), label_y, _spectrum_rate.shown, spectrum_color);
    draw_number(*canvas, scope_rect.left(), label_y, _scope_rate.shown, _scope.triggered ? scope_color : free_run_color);

    // Gain strip.
    fill(*canvas, strip_rect, colors.panel);
    const auto gain_h = static_cast<float>(_gain) * strip_rect.height();
    fill(*canvas, SkRect::MakeXYWH(strip_rect.x(), strip_rect.bottom() - gain_h, strip_rect.width(), gain_h), colors.text);

    canvas->restore();
}

} // namespace tiny::edit
