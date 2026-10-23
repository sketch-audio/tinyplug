#include "editor.hpp"

#include <tiny_ui/draw.hpp>
#include <tiny_platform/platform_dialogs.hpp>
#include <tiny_platform/platform_text.hpp>

#if !TINY_PLATFORM_WASM
#include "include/gpu/ganesh/GrRecordingContext.h"
#endif

#include <algorithm>

namespace tiny::edit {

namespace {

constexpr auto raw_lines = size_t{9};
constexpr auto dialog_names = std::array<const char*, 10>{"MESSAGE", "CONFIRM", "TEXT > GAIN", "CHAINED", "OPEN FILE", "SAVE FILE", "CHOOSE DIR", "OPEN URL", "INLINE > GAIN", "INLINE NOTE"};
constexpr auto dialog_rows = 5;

auto point(Coords p) -> std::string { return std::to_string(static_cast<int>(p.x)) + "," + std::to_string(static_cast<int>(p.y)); }

auto button_name(Pointer::Button b) -> std::string { return b == Pointer::Button::left ? "L" : "R"; }

auto describe(const Event& event) -> std::string
{
    const auto tag = " #" + std::to_string(event.pointer_tag % 1000);
    return std::visit(Inline_visitor{
        [&](const Pointer::Down& e) { return "DOWN " + button_name(e.button) + " " + point(e.pos) + tag; },
        [&](const Pointer::Up& e) { return "UP " + button_name(e.button) + " " + point(e.pos) + tag; },
        [&](const Pointer::Move& e) { return "MOVE " + point(e.pos) + tag; },
        [&](const Pointer::Click& e) { return "CLICK " + button_name(e.button) + " X" + std::to_string(e.count) + " " + point(e.pos) + tag; },
        [&](const Pointer::Enter& e) { return "ENTER " + point(e.pos) + tag; },
        [&](const Pointer::Exit& e) { return "EXIT " + point(e.pos) + tag; },
        [&](const Pointer::Cancel& e) { return "CANCEL " + point(e.pos) + tag; },
    }, event.event);
}

auto format_name(Format f) -> const char*
{
    switch (f) {
        case Format::Aax: return "AAX";
        case Format::Auv2: return "AUV2";
        case Format::Auv3: return "AUV3";
        case Format::Clap: return "CLAP";
        case Format::Vst3: return "VST3";
        case Format::Web: return "WEB";
    }
    return "?";
}

auto backend_name(SkCanvas& canvas) -> const char*
{
#if TINY_PLATFORM_WASM
    (void)canvas;
    return "CPU (RASTER)"; // The web build's Skia has no Ganesh.
#else
    const auto* gpu = canvas.recordingContext();
    if (!gpu) return "CPU (RASTER)";
    switch (gpu->backend()) {
        case GrBackendApi::kMetal: return "GPU METAL";
        case GrBackendApi::kDirect3D: return "GPU D3D";
        case GrBackendApi::kOpenGL: return "GPU OPENGL";
        case GrBackendApi::kVulkan: return "GPU VULKAN";
        default: return "GPU OTHER";
    }
#endif
}

} // namespace

auto Editor::_make_probes() -> void
{
    // Each callback lights its probe's lamp and names the phase.
    const auto callbacks = [this]<typename Info>(size_t i, std::function<void(const Info&)> extra = {}) {
        const auto hit = [this, i](const char* phase) { auto& p = _probes[i]; p.phase = phase; p.flash = 0.25; };
        return gesture::Callbacks<Info>{
            .on_started = [=, this](const Info& info) { hit("STARTED"); ++_probes[i].count; if (extra) extra(info); },
            .on_updated = [=](const Info& info) { hit("UPDATED"); if (extra) extra(info); },
            .on_ended = [=, this](const Info& info) { hit("ENDED"); if (extra) extra(info); if constexpr (std::is_same_v<Info, gesture::Drag::Info>) _drag.reset(); },
            .on_cancelled = [=, this]() { hit("CANCELLED"); if constexpr (std::is_same_v<Info, gesture::Drag::Info>) _drag.reset(); },
        };
    };

    _probes[0] = {"OVER", std::make_unique<gesture::Over>(callbacks.template operator()<gesture::Over::Info>(0))};
    _probes[1] = {"DOWN", std::make_unique<gesture::Down>(callbacks.template operator()<gesture::Down::Info>(1))};
    _probes[2] = {"DWELL 2S", std::make_unique<gesture::Dwell>(callbacks.template operator()<gesture::Dwell::Info>(2))};
    _probes[3] = {"CLICK L", std::make_unique<gesture::Click>(callbacks.template operator()<gesture::Click::Info>(3), gesture::Click::Desc{})};
    _probes[4] = {"DOUBLE L", std::make_unique<gesture::Click>(callbacks.template operator()<gesture::Click::Info>(4), gesture::Click::Desc{.count = 2})};
    _probes[5] = {"CLICK R", std::make_unique<gesture::Click>(callbacks.template operator()<gesture::Click::Info>(5), gesture::Click::Desc{.button = Pointer::Button::right})};
    _probes[6] = {"DRAG", std::make_unique<gesture::Drag>(callbacks.template operator()<gesture::Drag::Info>(6, [this](const gesture::Drag::Info& d) { _drag = d; }), false, true, 3.)};
}

auto Editor::on_hide() -> void
{
    _controls.cancel(_edit.params);
    platform::Text::cancel(_window);
}

auto Editor::_dialog(int which, const Frame& gain, const Frame& note, double gain_knob) -> void
{
    const auto ctx = platform::Context{_edit.tasks, _window};
    const auto report = [this](std::string what) { _result = std::move(what); };
    const auto t = ui::theme(_dark);
    switch (which) {
        case 0: platform::Dialogs::message("Message", "One button. The callback fires once, however it closes.", [=]() { report("MESSAGE CLOSED"); }, ctx); break;
        case 1: platform::Dialogs::confirm("Confirm", "OK or Cancel: both answer.", [=](bool ok) { report(ok ? "CONFIRM: OK" : "CONFIRM: CANCEL"); }, ctx); break;
        case 2:
            platform::Dialogs::text_input("Gain", "A value between 0 and 1. This prompt is long on purpose, so the dialog has to wrap it rather than grow to fit it on one line.", [this, report](std::string text) {
                if (text.empty()) return report("TEXT: CANCELLED");
                const auto address = enum_raw(Address::Gain);
                const auto& spec = User_params::param_spec(address);
                const auto value = params::from_text(text, spec.semantics);
                if (!value) return report("TEXT: NOT A NUMBER");
                _edit.params.start(address);
                _edit.params.set(address, params::plain_to_knob(*value, spec.semantics));
                _edit.params.end(address);
                report("TEXT: GAIN = " + text);
            }, ctx);
            break;
        case 3: // The second is asked for while the first is still closing: it must queue, not vanish.
            platform::Dialogs::confirm("Chained", "Answer, and a second dialog should follow.", [this, ctx, report](bool ok) {
                platform::Dialogs::message(ok ? "Second dialog" : "Second, after cancel", "It queued behind the first.", [=]() { report("CHAINED: BOTH SHOWN"); }, ctx);
            }, ctx);
            break;
        case 4: platform::Dialogs::open_file("Open", "", [=](std::optional<std::string> p) { report(p ? "OPEN: " + *p : "OPEN: CANCELLED"); }, ctx); break;
        case 5: platform::Dialogs::save_file("Save", "", "untitled", "txt", [=](std::optional<std::string> p) { report(p ? "SAVE: " + *p : "SAVE: CANCELLED"); }, ctx); break;
        case 6: platform::Dialogs::choose_dir("Choose", "", [=](std::optional<std::string> p) { report(p ? "DIR: " + *p : "DIR: CANCELLED"); }, ctx); break;
        case 7: platform::Dialogs::open_url("https://github.com/sketch-audio/tinyplug", ctx); report("URL: REQUESTED"); break;
        case 8: { // A native field over the slider: Enter applies it as one gesture, Escape or clicking away leaves it.
            const auto address = enum_raw(Address::Gain);
            const auto& spec = User_params::param_spec(address);
            const auto field = platform::Text::Field{
                .frame = gain,
                .initial = params::to_text(params::knob_to_plain(gain_knob, spec.semantics), spec.semantics),
                .label = spec.name,
                .ink = t.text,
                .background = t.panel,
                .blur = platform::Text::Field::Blur::Cancel,
            };
            platform::Text::edit(field, [this, report](platform::Text::Result r) {
                if (!r.committed()) return report("INLINE: CANCELLED");
                const auto address = enum_raw(Address::Gain);
                const auto& spec = User_params::param_spec(address);
                const auto value = params::from_text(r.text, spec.semantics);
                if (!value) return report("INLINE: NOT A NUMBER");
                _edit.params.start(address);
                _edit.params.set(address, params::plain_to_knob(params::clamp(*value, spec.semantics), spec.semantics));
                _edit.params.end(address);
                report("INLINE: GAIN = " + r.text);
            }, ctx);
            break;
        }
        case 9: { // Free text, left-aligned and not preselected; clicking away cancels.
            const auto field = platform::Text::Field{
                .frame = note,
                .initial = "",
                .label = "Note",
                .ink = ui::amber,
                .background = t.panel,
                .align = platform::Text::Field::Align::Left,
                .select_all = false,
                .blur = platform::Text::Field::Blur::Cancel,
            };
            platform::Text::edit(field, [report](platform::Text::Result r) { report(r.committed() ? "INLINE: " + r.text : "INLINE: CANCELLED"); }, ctx);
            break;
        }
        default: break;
    }
}

auto Editor::on_draw(Plugin_state& state) -> void
{
    _dark = state.view_context.dark; // The host\'s or system\'s appearance, this frame.
    auto& view = state.view_context;
    auto* canvas = view.canvas;
    if (!canvas) return;

    const auto dt = _last == Time_point{} ? 0. : Durations::delta_secs(_last, view.time_now);
    _last = view.time_now;
    if (dt > 0) _fps += (1. / dt - _fps) * 0.1;

    const auto t = ui::theme(_dark);
    const auto& knobs = state.processor_state.params;
    auto& interaction = view.interaction;

    // Layout: the pad and its readouts left, dialogs and info right.
    const auto bounds = ui::inset({0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, 14);
    const auto left = Frame{bounds.x, bounds.y, bounds.w * 0.55, bounds.h};
    const auto right = Frame{left.x + left.w + 14, bounds.y, bounds.w - left.w - 14, bounds.h};
    const auto pad = Frame{left.x, left.y, left.w, 200};
    const auto lamps = Frame{left.x, pad.y + pad.h + 8, left.w, 84};
    const auto raw = Frame{left.x, lamps.y + lamps.h + 8, left.w, left.y + left.h - (lamps.y + lamps.h + 8)};
    const auto buttons = Frame{right.x, right.y, right.w, 190};
    const auto gain = Frame{right.x, buttons.y + buttons.h + 10, right.w, 24};
    const auto info = Frame{right.x, gain.y + gain.h + 10, right.w, right.y + right.h - (gain.y + gain.h + 10)};

    // Raw events first, before anything consumes them.
    for (const auto& event : interaction.events.events) {
        if (std::holds_alternative<Pointer::Move>(event.event) && !_raw.empty() && _raw.back().starts_with("MOVE")) _raw.pop_back(); // One line per run of moves.
        _raw.push_back(describe(event));
        if (_raw.size() > raw_lines) _raw.pop_front();
    }
    _scroll.x += interaction.scroll_deltas.x;
    _scroll.y += interaction.scroll_deltas.y;

    const auto controls = std::array<ui::Control, 1>{{{gain, enum_raw(Address::Gain), ui::Control::Kind::Slider, "GAIN", ui::blue}}};
    _controls.interact(interaction, _edit.params, controls, knobs);
    for (auto& event : interaction.events.events) {
        const auto* down = std::get_if<Pointer::Down>(&event.event);
        if (!down || event.consumed || down->button != Pointer::Button::left) continue;
        for (auto i = 0; i < static_cast<int>(dialog_names.size()); ++i) {
            if (ui::row(ui::column(buttons, i % 2, 2, 8), i / 2, dialog_rows, 8).contains(down->pos)) { event.consumed = true; _dialog(i, gain, {info.x + 4, info.y + 2, info.w - 8, 18}, knobs[enum_raw(Address::Gain)]); }
        }
        if (lamps.contains(down->pos)) { _scroll = {}; for (auto& p : _probes) p.count = 0; } // Click the lamps to zero them.
    }

    if (_pad != pad) {
        _pad = pad;
        for (auto& p : _probes) p.recognizer->set_frame(pad);
    }
    for (auto& p : _probes) p.recognizer->process_events(interaction.events);

    canvas->save();
    canvas->scale(static_cast<float>(view.scale), static_cast<float>(view.scale));
    ui::fill(*canvas, {0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, t.background);

    // The pad, with the drag drawn across it.
    ui::fill(*canvas, pad, t.panel);
    ui::text_in(*canvas, pad, "CLICK, DOUBLE, RIGHT, DRAG, HOVER, DWELL", t.dim);
    if (_drag) {
        ui::line(*canvas, _drag->fpos, _drag->tpos, ui::amber, 2.f);
        ui::fill(*canvas, {_drag->tpos.x - 4, _drag->tpos.y - 4, 8, 8}, ui::amber);
    }

    // A lamp per recognizer, then modifiers and scroll. Click to zero the counts.
    for (auto i = size_t{}; i < _probes.size(); ++i) {
        auto& p = _probes[i];
        p.flash = std::max(0., p.flash - dt);
        const auto cell = ui::row(ui::column(lamps, static_cast<int>(i % 4), 4, 6), static_cast<int>(i / 4), 3, 4);
        ui::fill(*canvas, cell, p.flash > 0 ? ui::green : t.track);
        ui::text(*canvas, cell.x + 4, cell.y + 4, std::string{p.name} + " " + std::to_string(p.count), p.flash > 0 ? t.background : t.text);
    }
    const auto& keys = interaction.modifier_keys;
    const auto mods = std::array<std::pair<const char*, bool>, 3>{{{"PRIMARY", keys.primary}, {"ALT", keys.alt}, {"SHIFT", keys.shift}}};
    for (auto i = size_t{}; i < mods.size(); ++i) {
        const auto cell = ui::row(ui::column(lamps, static_cast<int>(i), 4, 6), 2, 3, 4);
        ui::fill(*canvas, cell, mods[i].second ? ui::blue : t.track);
        ui::text(*canvas, cell.x + 4, cell.y + 4, mods[i].first, mods[i].second ? t.background : t.text);
    }
    const auto scroll_cell = ui::row(ui::column(lamps, 3, 4, 6), 2, 3, 4);
    ui::fill(*canvas, scroll_cell, interaction.inertial_scroll ? ui::purple : t.track);
    ui::text(*canvas, scroll_cell.x + 4, scroll_cell.y + 4, "SCROLL " + point(_scroll) + (interaction.precise_scroll ? " P" : ""), t.text);

    // Raw pointer events, newest last.
    ui::fill(*canvas, raw, t.panel);
    auto y = raw.y + 6;
    for (const auto& line : _raw) { ui::text(*canvas, raw.x + 6, y, line, t.text); y += 12; }

    // Dialogs, the gain the text dialog sets, and the last result.
    for (auto i = 0; i < static_cast<int>(dialog_names.size()); ++i) {
        const auto cell = ui::row(ui::column(buttons, i % 2, 2, 8), i / 2, dialog_rows, 8);
        ui::fill(*canvas, cell, t.track);
        ui::text_in(*canvas, cell, dialog_names[static_cast<size_t>(i)], t.text);
    }
    ui::draw_controls(*canvas, t, controls, knobs);

    ui::fill(*canvas, info, t.panel);
    const auto lines = std::array<std::string, 8>{
        _result.substr(0, 38),
        "",
        std::string{"FORMAT "} + format_name(_edit.format),
        std::string{"GRAPHICS "} + backend_name(*canvas),
        "SCALE " + ui::fixed(view.scale, 2) + "  DARK " + (_dark ? "YES" : "NO"),
        "SIZE " + std::to_string(view.logical_size.w) + "X" + std::to_string(view.logical_size.h) + "  PX " +
            std::to_string(static_cast<int>(view.logical_size.w * view.scale)) + "X" + std::to_string(static_cast<int>(view.logical_size.h * view.scale)),
        "FPS " + ui::fixed(_fps, 0) + "  POINTERS HELD " + std::to_string(interaction.events.pointer_origins.size()),
        "POINTER " + point(interaction.pointer_abs),
    };
    y = info.y + 8;
    for (auto i = size_t{}; i < lines.size(); ++i) { ui::text(*canvas, info.x + 8, y, lines[i], i == 0 ? ui::amber : t.text); y += 14; }

    canvas->restore();
}

} // namespace tiny::edit
