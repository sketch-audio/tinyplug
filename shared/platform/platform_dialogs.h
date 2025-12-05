#pragma once

#include <string>

#include "../tinyplug/tiny_edit.h" // Later

namespace tiny {

struct Platform_dialogs {
    // Callback is pushed into the executor's task queue. You will need to drain this queue somewhere, probably on the main thread!
    static auto message(const std::string& title, const std::string& message, std::function<void()> on_done, Execution_context executor) -> void;

    // Callback is pushed into the executor's task queue. You will need to drain this queue somewhere, probably on the main thread!
    static auto confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_done, Execution_context executor) -> void;

    // Callback is pushed into the executor's task queue. You will need to drain this queue somewhere, probably on the main thread!
    static auto text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Execution_context executor) -> void;

    // On Windows we use the executor to launch the browser from a background thread.
    static auto open_url(const std::string& url, Execution_context executor) -> void;

    // Callback runs automatically in the executor's task launcher (some background thread). Because of sandboxing requirements on iOS, we can only guarantee access to the file during your callback scope! You are responsible for escaping back to main safely.
    static auto open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Execution_context executor) -> void;
};

} // namespace tiny