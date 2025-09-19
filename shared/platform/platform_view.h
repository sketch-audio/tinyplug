#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include "platform.h"
#include "view_delegate.h"

#include "../tinyplug/tinyplug.h"

namespace tiny {

#if PLATFORM_WINDOWS
// State binder for window callback.
struct Platform_binder {
    View_delegate* delegate{};
    User_interaction interaction{};
    Pointer pointer{};
    std::chrono::steady_clock::time_point over_time{};
    std::optional<Coords> over_pos{};
    std::optional<Coords> left_pos{};
    std::optional<Coords> right_pos{};
    std::optional<Coords> drag_start{};
    bool dwelt{};
    bool dark_mode{};
};
class Dark_mode_watcher; // Have to manually watch dark mode.
class Vsync_loop; // Vsync drawing.
#endif

struct Platform_view {
    // Use the factory.
    Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()> on_release = []() {});
    ~Platform_view();

    auto on_create() -> void;
    auto on_show() -> void;
    auto on_hide() -> void;
    auto on_destroy() -> void;

    auto receive_parent(void* parent) -> void;
    auto resize(int32_t w, int32_t h) -> void;
    
    auto native_handle() -> void* { return _view; }
    auto get_size() -> Rect_size { return _delegate->get_size(); }
private:
    const bool _owns_view{true};
    std::shared_ptr<View_delegate> _delegate;
    void* _view{nullptr};

#if PLATFORM_WINDOWS
    Platform_binder _binder{};
    std::unique_ptr<Dark_mode_watcher> _dark_watcher{nullptr};
    std::unique_ptr<Vsync_loop> _vsync_loop{nullptr};
#endif
};

// Factory
struct Platform_views {
    static auto make_owning(std::shared_ptr<View_delegate> delegate) -> std::unique_ptr<Platform_view> {
        return std::make_unique<Platform_view>(delegate, true);
    }

#if PLATFORM_MACOS || PLATFORM_IOS
    static auto make_autoreleasing(std::shared_ptr<View_delegate> delegate, std::function<void()> on_autorelease) -> std::unique_ptr<Platform_view> {
        return std::make_unique<Platform_view>(delegate, false, on_autorelease);
    }
#endif
};

struct Platform_dialogs {
    static auto message(const std::string& title, const std::string& message, Later<> on_done = {}) -> void;
    static auto confirm(const std::string& title, const std::string& message, Later<bool> on_done = {}) -> void;
    static auto text_input(const std::string& title, const std::string& message, Later<std::string> on_text = {}) -> void;
    static auto open_url(const std::string& url) -> void;
};

} // namespace tiny