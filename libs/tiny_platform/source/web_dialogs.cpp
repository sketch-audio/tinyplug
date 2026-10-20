// Browser dialogs: alert / confirm / prompt for now, which block the page as a native sheet blocks its window;
// HTML overlays are the follow-up. Files have no picker yet, so they report cancelled.
#include <tiny_platform/platform_dialogs.hpp>

#include <emscripten/emscripten.h>

namespace tiny {

namespace {

EM_JS(void, web_alert, (const char* title, const char* message), { alert(UTF8ToString(title) + "\n\n" + UTF8ToString(message)); });
EM_JS(int, web_confirm, (const char* title, const char* message), { return confirm(UTF8ToString(title) + "\n\n" + UTF8ToString(message)) ? 1 : 0; });
EM_JS(char*, web_prompt, (const char* title, const char* message), {
    const text = prompt(UTF8ToString(title) + "\n\n" + UTF8ToString(message));
    return text === null ? 0 : stringToNewUTF8(text);
});
EM_JS(void, web_open_url, (const char* url), { window.open(UTF8ToString(url), "_blank", "noopener"); });

// As the native dialogs: the answer arrives later, on the main thread.
auto later(const Dialog_context& ctx, std::function<void()> f) -> void
{
    if (!ctx.tasks.on_main(f)) f();
}

} // namespace

auto Platform_dialogs::message(const std::string& title, const std::string& message, std::function<void()> on_done, Dialog_context ctx) -> void
{
    web_alert(title.c_str(), message.c_str());
    if (on_done) later(ctx, std::move(on_done));
}

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_done, Dialog_context ctx) -> void
{
    const auto ok = web_confirm(title.c_str(), message.c_str()) != 0;
    if (on_done) later(ctx, [on_done, ok] { on_done(ok); });
}

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Dialog_context ctx) -> void
{
    auto* text = web_prompt(title.c_str(), message.c_str());
    if (!text) return; // Cancelled: no answer, as native.
    auto value = std::string{text};
    free(text);
    if (on_text) later(ctx, [on_text, value] { on_text(value); });
}

auto Platform_dialogs::open_url(const std::string& url, Dialog_context) -> void
{
    web_open_url(url.c_str());
}

auto Platform_dialogs::save_file(const std::string&, const std::string&, const std::string&, const std::string&, std::function<void(std::optional<std::string>)> on_save, Dialog_context ctx) -> void
{
    if (on_save) later(ctx, [on_save] { on_save(std::nullopt); });
}

auto Platform_dialogs::open_file(const std::string&, const std::string&, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void
{
    if (on_open) later(ctx, [on_open] { on_open(std::nullopt); });
}

auto Platform_dialogs::choose_dir(const std::string&, const std::string&, std::function<void(std::optional<std::string>)> on_choose, Dialog_context ctx) -> void
{
    if (on_choose) later(ctx, [on_choose] { on_choose(std::nullopt); });
}

} // namespace tiny
