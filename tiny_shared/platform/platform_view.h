#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include "platform.h"
#include "graphics_delegate.h"

namespace tiny {

#if PLATFORM_WINDOWS
// State binder for window callback.
struct Platform_binder {
    View_delegate* delegate{};
    User_interaction interaction{};
    std::chrono::steady_clock::time_point over_time{};
    std::optional<Coords> over_pos{};
    std::optional<Coords> left_pos{};
    std::optional<Coords> right_pos{};
    std::optional<Coords> drag_start{};
    bool dwelt{};
};
#endif

struct Platform_view {
    Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view);
    ~Platform_view();
    auto receive_parent(void* parent) -> void;
    auto teardown() -> void; // You might not get this.
    auto resize(int32_t w, int32_t h) -> void;
    auto redraw() -> void;
    auto native_handle() -> void* { return _view; }
    auto get_size() -> Rect_size { return _delegate->get_size(); }
private:
    const bool _owns_view{true};
    std::shared_ptr<View_delegate> _delegate;
    void* _view{nullptr};

#if PLATFORM_WINDOWS
    Platform_binder _binder{};
#endif
};

// Factory
struct Platform_views {
    static auto make_owning(std::shared_ptr<View_delegate> delegate) -> std::unique_ptr<Platform_view> {
        return std::make_unique<Platform_view>(delegate, true);
    }

#if PLATFORM_MACOS
    static auto make_autoreleasing(std::shared_ptr<View_delegate> delegate) -> std::unique_ptr<Platform_view> {
        return std::make_unique<Platform_view>(delegate, false);
    }
#endif
};

} // namespace tiny