#include "plug_editor.h"

#include "include/core/SkCanvas.h"

#include "platform/platform_dialogs.h"

namespace tiny {

auto Plug_editor::on_gui_create() -> void
{
}

auto Plug_editor::on_gui_show(const Edit_context& edit) -> void
{
    _edit = edit;

    _click = std::make_unique<Click_recognizer>(Gesture_callbacks<Click_info>{
        .on_started = [this](const Click_info& info) {
            // Providing an execution context makes sure the dialog result is handled on the main thread.
            Platform_dialogs::text_input("Gain", "Enter a value between 0 and 1.", [this](std::string text) {
                const auto addr = enum_raw(Param_address::gain);
                const auto& param_spec = User_params::param_spec(addr);
                if (const auto value = Host_formatter::format_value(text, param_spec.semantics)) {
                    const auto knob = Value_conv::plain_to_knob(*value, param_spec.semantics);
                    _edit.actions.push(Action_start{addr});
                    _edit.actions.push(Set_param{addr, knob});
                    _edit.actions.push(Action_end{addr});
                }
            }, {_launcher.actor(), _main_queue.actor()});
        },
        .on_updated = [](const Click_info&) {},
        .on_ended = [](const Click_info&) {},
        .on_cancelled = []() {}
    }, Click_recognizer::Desc{/* single, left click */});
}

auto Plug_editor::on_gui_draw(Plugin_state& state) -> void
{
    // Update (Send actions).
    auto& view_context = state.view_context;
    const auto lsize = view_context.logical_size;
    auto frame = Frame{.x = 0, .y = 0, .w = static_cast<double>(lsize.w), .h = static_cast<double>(lsize.h)};
    if (_frame != frame) {
        _frame = frame;
        if (_click)
            _click->set_frame(_frame);
    }

    const auto& param_values = state.processor_state.params;
    const auto addr = enum_raw(Param_address::gain);
    _value = param_values[addr];
    if (_click)
        _click->process_events(view_context.interaction.events);

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
    const auto g_h = _value * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, static_cast<float>(rsize.w), g_h), paint);

    _main_queue.execute_all(); // Drain queue.
}

auto Plug_editor::on_gui_notify(const Ui_notification& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& n) { _dark = n.new_value; },
        [](const auto&) {}
    }, notification);
}

auto Plug_editor::on_gui_hide() -> void
{
}

auto Plug_editor::on_gui_destroy() -> void
{
}

} // namespace tiny