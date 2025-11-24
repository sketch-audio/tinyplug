#pragma once

#include <string>

#include "../tinyplug/tiny_edit.h" // Later

namespace tiny {

struct Platform_dialogs {
    static auto message(const std::string& title, const std::string& message, Later<> on_done = {}) -> void;
    static auto confirm(const std::string& title, const std::string& message, Later<bool> on_done = {}) -> void;
    static auto text_input(const std::string& title, const std::string& message, Later<std::string> on_text = {}) -> void;
    static auto open_url(const std::string& url) -> void;
    static auto open_file(const std::string& title, const std::string& default_path, Later<std::optional<std::string>> on_open = {}) -> void;
};

} // namespace tiny