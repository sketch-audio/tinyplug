#include "editor.hpp"

#include "include/core/SkCanvas.h"

#include <tiny_platform/platform_dialogs.hpp>

namespace tiny::plugin {

auto Editor::on_gui_create(Gui_info info) -> void
{
    // Names the window this editor just got. Passed to every dialog below so
    // they hang off *our* window rather than whatever the host has key.
    _window = info.window;
}

auto Editor::_make_gestures() -> void
{
    using namespace params;

    // Left click: a single prompt, parented to our own window.
    _click = std::make_unique<Click_recognizer>(Gesture_callbacks<Click_info>{
        .on_started = [this](const Click_info&) {
            // Providing an execution context makes sure the dialog result is handled on the main thread.
            Platform_dialogs::text_input("Gain", "Enter a value between 0 and 1. This prompt is deliberately long so that the dialog must word-wrap rather than stretch to fit the entire message on a single line — useful for verifying the Windows auto-wrap behavior matches macOS and iOS.", [this](std::string text) {
                if (text.empty()) return; // Cancelled — the callback still fires.
                const auto addr = enum_raw(Address::Gain);
                const auto& param_spec = User_params::param_spec(addr);
                if (const auto value = Host_formatter::to_value(text, param_spec.semantics)) {
                    const auto knob = Value_helper::plain_to_knob(*value, param_spec.semantics);
                    _edit.actions.push(Action_start{addr});
                    _edit.actions.push(Set_param{addr, knob});
                    _edit.actions.push(Action_end{addr});
                }
            }, Dialog_context{_tasks, _window});
        },
        .on_updated = [](const Click_info&) {},
        .on_ended = [](const Click_info&) {},
        .on_cancelled = []() {}
    }, Click_recognizer::Desc{/* single, left click */});

    // Right click: two dialogs back to back. The second is requested from the
    // first's callback, so it arrives while the first sheet is still on screen —
    // the case that used to swallow the second dialog entirely.
    _chain = std::make_unique<Click_recognizer>(Gesture_callbacks<Click_info>{
        .on_started = [this](const Click_info&) {
            Platform_dialogs::confirm("Chained dialogs", "Say OK and a second dialog should follow immediately.", [this](bool confirmed) {
                Platform_dialogs::message(
                    confirmed ? "Second dialog" : "Cancelled",
                    confirmed ? "If you can read this, the sheet queued behind the first one instead of being dropped."
                              : "Cancel answers too — it no longer drops the callback.",
                    []() {}, Dialog_context{_tasks, _window});
            }, Dialog_context{_tasks, _window});
        },
        .on_updated = [](const Click_info&) {},
        .on_ended = [](const Click_info&) {},
        .on_cancelled = []() {}
    }, Click_recognizer::Desc{.button = Pointer_button::right});
}

auto Editor::on_gui_show() -> void
{
}

auto Editor::on_gui_draw(Plugin_state& state) -> void
{
    // Update (Send actions).
    auto& view_context = state.view_context;
    const auto lsize = view_context.logical_size;
    auto frame = Frame{.x = 0, .y = 0, .w = static_cast<double>(lsize.w), .h = static_cast<double>(lsize.h)};
    if (_frame != frame) {
        _frame = frame;
        if (_click)
            _click->set_frame(_frame);
        if (_chain)
            _chain->set_frame(_frame);
    }

    const auto& param_values = state.processor_state.params;
    const auto addr = enum_raw(Address::Gain);
    _value = param_values[addr];
    if (_click)
        _click->process_events(view_context.interaction.events);
    if (_chain)
        _chain->process_events(view_context.interaction.events);

    // Draw.
    auto* canvas = view_context.canvas;

    // Calculate real size.
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(_dark ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);

    // Draw gain value.
    paint.setColor(_dark ? SK_ColorWHITE : SK_ColorBLACK);
    const auto g_h = static_cast<float>(_value * rsize.h);
    const auto g_y = static_cast<float>(rsize.h - g_h);
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, static_cast<float>(rsize.w), g_h), paint);
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
}

auto Editor::on_gui_destroy() -> void
{
    // Deliberately does *not* clear `_window`. The registry is the authority on
    // liveness — `~Platform_view` retires the token, so a stale one already
    // resolves to nothing. Clearing here would add nothing and can actively
    // hurt: AUv2 runs this teardown from the view's `dealloc`, i.e. whenever the
    // autorelease pool drains, which can be *after* the next `on_gui_create` has
    // handed us a live token.
}

} // namespace tiny::plugin