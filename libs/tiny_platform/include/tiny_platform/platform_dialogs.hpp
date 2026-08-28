#pragma once

#include <functional>
#include <optional>
#include <string>

#include <tinyplug/task_manager.hpp>
#include <tinyplug/window_token.hpp>

namespace tiny {

// Where a dialog runs its callback and which window it hangs off.
//
// `window` names the view that asked. Without it the platform layer has to guess
// (the host's key window on macOS, a window-class scan on Windows), and in hosts
// that keep focus on their own window — Logic being the one that bit us — the
// guess is wrong and the dialog appears somewhere the user cannot see.
//
// Construction is explicit: naming no window is a real decision (a controller with
// no editor, say), not something a call site should fall into by passing the wrong
// handle. Editors pass the token they were given in `on_gui_create`.
struct Dialog_context {
    Task_manager::Actor tasks{};
    Window_token window{};

    Dialog_context() = default;
    explicit Dialog_context(Task_manager::Actor t, Window_token w = {}) : tasks{t}, window{w} {}
};

struct Platform_dialogs {
    // Callbacks below are pushed into the executor's task queue. You will need to drain this queue somewhere, probably on the main thread!
    //
    // Every one of them fires exactly once, on every path — including "the user
    // cancelled" and "there was no window to present on". A dialog that cannot be
    // shown degrades to a cancel rather than leaving the caller waiting forever.

    static auto message(const std::string& title, const std::string& message, std::function<void()> on_done, Dialog_context ctx) -> void;

    static auto confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_done, Dialog_context ctx) -> void;

    // `on_text` receives an empty string on cancel (and if the dialog could not be shown).
    static auto text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Dialog_context ctx) -> void;

    // On Windows we use the executor to launch the browser from a background thread.
    static auto open_url(const std::string& url, Dialog_context ctx) -> void;

    // Callback runs automatically in the executor's task launcher (some background thread). Because of sandboxing requirements on iOS, we can only guarantee access to the file during your callback scope! You are responsible for escaping back to main safely.
    static auto save_file(const std::string& title, const std::string& default_path, const std::string& name, const std::string& extension, std::function<void(std::optional<std::string>)> on_save, Dialog_context ctx) -> void;

    // Callback runs automatically in the executor's task launcher (some background thread). Because of sandboxing requirements on iOS, we can only guarantee access to the file during your callback scope! You are responsible for escaping back to main safely.
    static auto open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void;

    // Same as `open_file`, but restricted to picking a folder instead of a file.
    static auto choose_dir(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_choose, Dialog_context ctx) -> void;
};

} // namespace tiny
