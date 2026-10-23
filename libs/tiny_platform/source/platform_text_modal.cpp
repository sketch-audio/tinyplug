// Platform_text where there's no native inline field yet (iOS, Windows): the modal text input, titled with the
// field's label, the initial text as its message (it can't prefill). An empty answer reads as cancelled, as the dialog
// reports one.
#include <tiny_platform/platform_text.hpp>

namespace tiny {

auto Platform_text::edit(const Text_field& field, std::function<void(Text_result)> on_done, Dialog_context ctx) -> void
{
    Platform_dialogs::text_input(field.label, field.initial, [on_done = std::move(on_done)](std::string text) {
        if (!on_done) return;
        if (text.empty()) on_done({.kind = Text_result::Kind::Cancelled});
        else on_done({.kind = Text_result::Kind::Committed, .text = std::move(text)});
    }, ctx);
}

auto Platform_text::cancel(Window_token) -> void {}

} // namespace tiny
