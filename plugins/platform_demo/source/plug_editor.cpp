#include "plug_editor.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "platform/platform_view.h" // For Platform_dialogs.

namespace tiny {

auto Plug_editor::on_gui_show(const View_connection& connection) -> void
{
    _actions = connection.actions;
    _task_receiver = connection.tasks;
}

auto Plug_editor::on_gui_draw(App_state& app_state) -> void
{
    auto& processor_state = app_state.processor_state;
    auto& view_context = app_state.view_context;

    auto& interaction = view_context.interaction;
    auto* canvas = view_context.canvas;

    // Resolve the real size.
    const auto lsize = view_context.logical_size;
    const auto scale = view_context.scale;
    const auto rsize = Rect_size{
        .w = static_cast<int32_t>(lsize.w * scale),
        .h = static_cast<int32_t>(lsize.h * scale)
    };

    // Get param, export values.
    auto& params = processor_state.params;
    auto& meters = processor_state.meters;

    const auto id = enum_raw(Param_id::gain);
    const auto g = static_cast<float>(params[id]);

    const auto& spec = _params.param_for(id);

    // Get incremented parameter value from a drag.
    auto get_next = [=, this](auto x, auto& d) -> double {
        const auto drag_y = d.tpos.y - d.fpos.y;
        const auto drag_dy = drag_y - _ldrag;
        const auto norm_dy = drag_dy / lsize.h;
        return std::clamp(x - norm_dy, double{}, double{1});
    };

    for (auto& pointer : interaction.pointers) {
        if (_pointer && *_pointer != pointer.tag) continue; // Haven't bound yet.

        // Handle user actions.
        std::visit(Inline_visitor{
            [&](const Down&) { _pointer = pointer.tag; }, // Bind on down.
            [&](const Drag_start& s) {
                _ldrag = {};
                const auto to_set = get_next(g, s);
                _actions.push(Action_start{id});
                _actions.push(Set_param{id, to_set});
                _ldrag = s.tpos.y - s.fpos.y;
            },
            [&](const Drag& s) {
                const auto to_set = get_next(g, s);
                _actions.push(Set_param{id, to_set});
                _ldrag = s.tpos.y - s.fpos.y;
            },
            [&](const Drag_end& s) {
                const auto to_set = get_next(g, s);
                _actions.push(Set_param{id, to_set});
                _actions.push(Action_end{id});
                _ldrag = {};
                _pointer = std::nullopt;
            },
            [&](const Click& c) {
                if (interaction.modifier_keys.primary) {
                    Platform_dialogs::message("Tiny Demo", "This is a message dialog.");
                }
                else if (interaction.modifier_keys.shift) {
                    Platform_dialogs::confirm("Are you sure?", "This is a confirm dialog.", {
                        .callback = [](bool confirmed) { std::cout << "User confirmed: " << (confirmed ? "yes" : "no") << "\n"; },
                        .receiver = _task_receiver
                    });
                }
                else if (interaction.modifier_keys.alt) {
                    Platform_dialogs::text_input("TinyDemo", "This is a text input dialog.", {
                        .callback = [this](auto text) {
                            const auto& param = _params.param_for(enum_raw(Param_id::gain));
                            if (const auto value = Host_formatter::format_value(text, param.semantics)) {
                                const auto knob = Value_conv::plain_to_knob(*value, param.semantics);
                                _actions.push(Action_start{param.id});
                                _actions.push(Set_param{param.id, knob});
                                _actions.push(Action_end{param.id});
                            }
                        },
                        .receiver = _task_receiver 
                    });
                }
                else {
                    //Platform_dialogs::open_url("https://www.sketchaudio.com");
                }
                _pointer = std::nullopt;
            },
            [&](const Double_click&) {
                Platform_dialogs::text_input("TinyDemo", "This is a text input dialog.", {
                    .callback = [this](auto text) {
                        const auto& param = _params.param_for(enum_raw(Param_id::gain));
                        if (const auto value = Host_formatter::format_value(text, param.semantics)) {
                            const auto knob = Value_conv::plain_to_knob(*value, param.semantics);
                            _actions.push(Action_start{param.id});
                            _actions.push(Set_param{param.id, knob});
                            _actions.push(Action_end{param.id});
                        }
                    },
                    .receiver = _task_receiver
                });
                _pointer = std::nullopt;
            },
            [&](const Right_click& c) {
                Platform_dialogs::message("Tiny Demo", "A right click occurred.");
                _pointer = std::nullopt;
            },
            [](const auto&) {}
        }, pointer.state);
        track_is_down(pointer.state, _down); // Track down.
    }

    // Draw background.
    auto paint = SkPaint{};
    paint.setColor(_dark ? SK_ColorBLACK : SK_ColorWHITE);
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(rsize.w), static_cast<float>(rsize.h)), paint);

    // Draw gain value.
    paint.setColor(_dark ? SK_ColorWHITE : SK_ColorBLACK);
    if (interaction.modifier_keys.any() || _down) {
        paint.setColor(SK_ColorBLUE);
    }

    const auto g_h = g * rsize.h;
    const auto g_y = rsize.h - g_h;
    canvas->drawRect(SkRect::MakeXYWH(0, g_y, static_cast<float>(rsize.w), g_h), paint);
}

auto Plug_editor::on_gui_notify(const Ui_notification& notification) -> void
{
    std::visit(Inline_visitor{
        [&](const Dark_mode_changed& d) { _dark = d.new_value; },
        [](const auto&) {}
    }, notification);
}

} // namespace tiny