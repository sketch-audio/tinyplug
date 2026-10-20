// Browser dialogs: alert / confirm / prompt, which block the page as a native sheet blocks its window. Files go
// through the browser: open copies the picked file into the virtual filesystem, save downloads what the editor
// wrote to a scratch path. Both need the page's user activation, which the click that asked for them provides.
#include <tiny_platform/platform_dialogs.hpp>

#include <cstdint>
#include <map>
#include <mutex>

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

// Pickers answer from a DOM event, long after the call: their callbacks wait here by id.
struct Pending {
    std::function<void(std::optional<std::string>)> on_path{};
    Dialog_context ctx{};
};
std::mutex pending_mutex{};
std::map<std::uint32_t, Pending> pending{};
std::uint32_t next_id{1};

auto park(std::function<void(std::optional<std::string>)> f, Dialog_context ctx) -> std::uint32_t
{
    const auto lock = std::lock_guard{pending_mutex};
    pending[next_id] = Pending{std::move(f), ctx};
    return next_id++;
}

EM_JS(void, web_open_file, (std::uint32_t id), {
    const input = document.createElement("input");
    input.type = "file";
    input.onchange = async () => {
        const file = input.files[0];
        if (!file) { _tiny_web_dialog_done(id, 0); return; }
        const dir = "/tinyplug/tmp";
        FS.mkdirTree(dir);
        const path = dir + "/" + file.name.replace(/[\/]/g, "_");
        FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
        const p = stringToNewUTF8(path);
        _tiny_web_dialog_done(id, p);
        _free(p);
    };
    input.oncancel = () => _tiny_web_dialog_done(id, 0);
    input.click();
});

EM_JS(void, web_download, (const char* path, const char* name), {
    const p = UTF8ToString(path);
    if (!FS.analyzePath(p).exists) return;
    const url = URL.createObjectURL(new Blob([FS.readFile(p)]));
    const a = Object.assign(document.createElement("a"), { href: url, download: UTF8ToString(name) });
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
});

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

// The editor writes to a scratch path in its callback; the file then downloads under its own name.
auto Platform_dialogs::save_file(const std::string&, const std::string&, const std::string& name, const std::string& extension, std::function<void(std::optional<std::string>)> on_save, Dialog_context ctx) -> void
{
    if (!on_save) return;
    const auto file = extension.empty() ? name : name + "." + extension;
    const auto path = "/tinyplug/tmp/" + file;
    later(ctx, [on_save, path, file] {
        EM_ASM({ FS.mkdirTree("/tinyplug/tmp"); });
        on_save(path);
        web_download(path.c_str(), file.c_str());
    });
}

auto Platform_dialogs::open_file(const std::string&, const std::string&, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void
{
    if (!on_open) return;
    web_open_file(park(std::move(on_open), ctx));
}

// The browser has no directories to choose: the plug-in's own persistent folder stands in.
auto Platform_dialogs::choose_dir(const std::string&, const std::string&, std::function<void(std::optional<std::string>)> on_choose, Dialog_context ctx) -> void
{
    if (on_choose) later(ctx, [on_choose] { on_choose(std::string{"/tinyplug/user"}); });
}

} // namespace tiny

extern "C" EMSCRIPTEN_KEEPALIVE auto tiny_web_dialog_done(std::uint32_t id, const char* path) -> void
{
    auto entry = tiny::Pending{};
    {
        const auto lock = std::lock_guard{tiny::pending_mutex};
        const auto it = tiny::pending.find(id);
        if (it == tiny::pending.end()) return;
        entry = std::move(it->second);
        tiny::pending.erase(it);
    }
    auto result = path ? std::optional<std::string>{path} : std::nullopt;
    tiny::later(entry.ctx, [f = std::move(entry.on_path), result] { f(result); });
}
