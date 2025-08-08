#pragma once

#include <memory>
#include <optional>

#include "platform.h"
#include "graphics_delegate.h"

namespace tiny {

struct Platform_view {
    Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view);
    ~Platform_view();
    auto receive_parent(void* parent) -> void;
    auto resize(int32_t w, int32_t h) -> void;
    auto redraw() -> void;
    auto native_handle() -> void* { return _view; }
    auto get_size() -> Rect_size { return _delegate->get_size(); }
private:
    const bool _owns_view{true};
    std::shared_ptr<View_delegate> _delegate;
    void* _view{nullptr};
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