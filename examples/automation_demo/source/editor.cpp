#include "editor.hpp"

#include <tiny_ui/draw.hpp>

#include <algorithm>

namespace tiny::edit {

namespace {

constexpr auto margin = 12.;
constexpr auto max_lines = size_t{400};
constexpr auto line_h = 13.;

constexpr auto param_names = std::array<const char*, 4>{"VALUE", "STEPS", "SWITCH", "DC OUT"};

auto color_of(models::Log_entry::Kind kind) -> SkColor
{
    using enum models::Log_entry::Kind;
    switch (kind) {
        case Set: return ui::blue;
        case Ramp: return ui::amber;
        case Latency: case Render: return ui::purple;
        case Configure: case Hard: case Soft: return ui::red;
    }
    return ui::red;
}

auto describe(const models::Log_entry& e) -> std::string
{
    using enum models::Log_entry::Kind;
    auto out = ui::pad("#" + std::to_string(e.seq), 7) + ui::pad(std::to_string(e.frame), 11) + "  ";
    const auto name = e.address < param_names.size() ? param_names[e.address] : "?";
    switch (e.kind) {
        case Configure: return out + "CONFIGURE  SR " + std::to_string(e.dur) + "  VALUE " + ui::fixed(e.value, 4);
        case Set: return out + "SET   " + name + " = " + ui::fixed(e.value, 4);
        case Ramp: return out + "RAMP  " + name + " > " + ui::fixed(e.value, 4) + " OVER " + std::to_string(e.dur);
        case Hard: return out + "RESET HARD  VALUE " + ui::fixed(e.value, 4);
        case Soft: return out + "RESET SOFT  VALUE " + ui::fixed(e.value, 4);
        case Latency: return out + "RESET LATENCY " + std::to_string(e.dur);
        case Render: return out + (e.value > 0.5 ? "RENDER OFFLINE" : "RENDER REALTIME");
    }
    return out;
}

} // namespace

auto Editor::notify(const Host_event& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Editor::_take(const models::Log_frame& log) -> void
{
    constexpr auto size = static_cast<uint32_t>(models::Log_frame::size);
    if (log.next <= _seen) { // The processor was rebuilt (AAX does at every reset): its count restarted.
        _lines.push_back({"-- PROCESSOR RESTARTED --", 0, models::Log_entry::Kind::Configure});
        _seen = 0;
    }
    const auto oldest = log.next > size ? log.next - size : 1u;
    if (_seen + 1 < oldest) _missed += oldest - (_seen + 1);
    for (auto seq = std::max(_seen + 1, oldest); seq < log.next; ++seq) {
        const auto& e = log.entries[seq % size];
        if (e.seq != seq) continue;
        _lines.push_back({describe(e), e.frame, e.kind});
    }
    _seen = log.next - 1;
    if (_lines.size() > max_lines) _lines.erase(_lines.begin(), _lines.begin() + static_cast<std::ptrdiff_t>(_lines.size() - max_lines));
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    auto& view = state.view_context;
    auto* canvas = view.canvas;
    if (!canvas) return;

    const auto t = ui::theme(_dark);
    const auto& knobs = state.processor_state.params;
    const auto& blocks = state.processor_state.blocks;
    if (const auto* log = blocks.fresh<Block::Log>()) _take(*log);
    const auto& trace = blocks.latest<Block::Trace>();

    // Layout, in logical points.
    const auto bounds = ui::inset({0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, margin);
    const auto controls_row = Frame{bounds.x, bounds.y, bounds.w, 26};
    const auto graph = Frame{bounds.x, controls_row.y + controls_row.h + margin, bounds.w, 150};
    const auto status = Frame{bounds.x, graph.y + graph.h + 6, bounds.w, 12};
    const auto log_panel = Frame{bounds.x, status.y + status.h + 6, bounds.w, bounds.y + bounds.h - (status.y + status.h + 6)};

    using Kind = ui::Control::Kind;
    const auto controls = std::array<ui::Control, 4>{{
        {ui::column(controls_row, 0, 4, 8), enum_raw(Address::Value), Kind::Slider, "VALUE", ui::green},
        {ui::column(controls_row, 1, 4, 8), enum_raw(Address::Steps), Kind::Slider, "STEPS", ui::blue},
        {ui::column(controls_row, 2, 4, 8), enum_raw(Address::Switch), Kind::Toggle, "SWITCH", ui::amber},
        {ui::column(controls_row, 3, 4, 8), enum_raw(Address::Dc_out), Kind::Toggle, "DC OUT", ui::red},
    }};
    _controls.interact(view.interaction, _edit.actions, controls, knobs);
    for (const auto& event : view.interaction.events.events) {
        if (const auto* down = std::get_if<Pointer_down>(&event.event); down && !event.consumed && log_panel.contains(down->pos)) {
            _lines.clear();
            _missed = 0;
        }
    }

    canvas->save();
    canvas->scale(static_cast<float>(view.scale), static_cast<float>(view.scale));
    ui::fill(*canvas, {0, 0, static_cast<double>(view.logical_size.w), static_cast<double>(view.logical_size.h)}, t.background);
    ui::draw_controls(*canvas, t, controls, knobs);

    // The realized Value, oldest on the left: one min / max bar per bucket.
    constexpr auto size = models::Trace_frame::size;
    ui::fill(*canvas, graph, t.panel);
    for (const auto level : {0., 0.5, 1.}) {
        const auto y = graph.y + graph.h * (1. - level);
        ui::line(*canvas, {graph.x, y}, {graph.x + graph.w, y}, t.track);
    }
    const auto bar_w = graph.w / size;
    for (auto i = size_t{}; i < size; ++i) {
        const auto slot = (trace.write + 1 + i) % size; // The bucket after the one being filled is the oldest.
        const auto lo = static_cast<double>(std::clamp(trace.lo[slot], 0.f, 1.f));
        const auto hi = static_cast<double>(std::clamp(trace.hi[slot], 0.f, 1.f));
        if (hi < lo) continue;
        const auto y = graph.y + graph.h * (1. - hi);
        ui::fill(*canvas, {graph.x + i * bar_w, y, std::max(bar_w, 1.), std::max(graph.h * (hi - lo), 1.5)}, ui::green);
    }

    // A tick where each logged event landed, if it's still on screen.
    const auto span = static_cast<double>(size) * trace.bucket;
    const auto start = static_cast<double>(trace.head) - static_cast<double>(size - 1) * trace.bucket;
    for (const auto& line : _lines) {
        const auto x = graph.x + (static_cast<double>(line.frame) - start) / span * graph.w;
        if (x < graph.x || x > graph.x + graph.w) continue;
        ui::line(*canvas, {x, graph.y}, {x, graph.y + 8}, color_of(line.kind), 2.f);
    }

    const auto value = static_cast<double>(trace.hi[trace.write == 0 ? size - 1 : trace.write - 1]);
    auto summary = "VALUE " + ui::fixed(value, 4) + "   EVENTS " + std::to_string(_seen) + "   ";
    if (_missed > 0) summary += "MISSED " + std::to_string(_missed) + "   ";
    summary += "FOUR SECONDS SHOWN";
    ui::text(*canvas, status.x, status.y, summary, t.dim);

    // The log, newest at the bottom.
    ui::fill(*canvas, log_panel, t.panel);
    ui::text(*canvas, log_panel.x + 6, log_panel.y + 6, "    SEQ      FRAME  EVENT   (CLICK TO CLEAR)", t.dim);
    const auto rows = static_cast<size_t>(std::max(0., (log_panel.h - 24) / line_h));
    const auto first = _lines.size() > rows ? _lines.size() - rows : 0;
    for (auto i = first; i < _lines.size(); ++i) {
        ui::text(*canvas, log_panel.x + 6, log_panel.y + 22 + static_cast<double>(i - first) * line_h, _lines[i].text, color_of(_lines[i].kind));
    }

    canvas->restore();
}

} // namespace tiny::edit
