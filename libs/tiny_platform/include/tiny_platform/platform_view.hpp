#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include <tiny_core/platform_defs.hpp>
#include <tiny_platform/view_delegate.hpp>

#include <tiny_core/tiny_core.hpp>

namespace tiny {
#if TINY_PLATFORM_WINDOWS
class Dark_mode_watcher; // Have to manually watch dark mode.
class Vsync_loop; // Vsync drawing.
#endif
} // namespace tiny

namespace tiny::platform {

#if TINY_PLATFORM_WINDOWS
// State binder for window callback.
struct Binder {
    View_delegate* delegate{};
    User_interaction interaction{};

    Event_stream events{};
    bool mouse_in{};
    Coords last_pos{};
    std::optional<Coords> left_down{};
    std::optional<Coords> right_down{};
    bool double_click{};

    bool dark_mode{};

    // Reentrancy guard.
    bool painting{};
};
#endif

// The editor's native view: an NSView, a UIView, an HWND, a <canvas>.
struct View {
    // Use the factories below.
    View(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()> on_release = []() {});
    ~View();

    static auto make_owning(std::shared_ptr<View_delegate> delegate) -> std::unique_ptr<View>
    {
        return std::make_unique<View>(delegate, true);
    }

#if TINY_PLATFORM_MACOS || TINY_PLATFORM_IOS
    static auto make_autoreleasing(std::shared_ptr<View_delegate> delegate, std::function<void()> on_autorelease) -> std::unique_ptr<View>
    {
        return std::make_unique<View>(delegate, false, on_autorelease);
    }
#endif

    auto on_create() -> void;
    auto on_show() -> void;
    auto on_hide() -> void;
    auto on_destroy() -> void;

    auto receive_parent(void* parent) -> void;
    auto resize(int32_t w, int32_t h) -> void;
    
    auto native_handle() -> void* { return _view; }
    auto get_size() -> Rect_size { return _delegate->get_size(); }

    // Names this view for as long as it exists. Thread it to `platform::Dialogs`
    // so a dialog hangs off the window that asked for it. Retired in the
    // destructor, after which resolving it yields nothing.
    auto token() const -> Window_token { return _token; }

private:
    const bool _owns_view{true};
    std::shared_ptr<View_delegate> _delegate;
    void* _view{nullptr};
    Window_token _token{};

#if TINY_PLATFORM_WINDOWS
    Binder _binder{};
    std::unique_ptr<Dark_mode_watcher> _dark_watcher{nullptr};
    std::unique_ptr<Vsync_loop> _vsync_loop{nullptr};
#endif
};

} // namespace tiny::platform
