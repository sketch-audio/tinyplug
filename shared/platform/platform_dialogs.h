#pragma once

#include <string>

#include "../tinyplug/tiny_edit.h" // Later

namespace tiny {

struct Platform_dialogs {
    static auto message(const std::string& title, const std::string& message, std::function<void()> on_done, Execution_context executor) -> void;
    static auto confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_done, Execution_context executor) -> void;
    static auto text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Execution_context executor) -> void;
    static auto open_url(const std::string& url) -> void;
    static auto open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Execution_context executor) -> void;
};

} // namespace tiny