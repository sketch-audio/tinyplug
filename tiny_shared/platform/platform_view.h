#pragma once

#include <memory>

#include "platform.h"
#include "graphics_delegate.h"

// AUv2
static const unsigned kAudioUnitProperty_UserPlugin = 'plug';

struct Platform_view {
    Platform_view(std::shared_ptr<Graphics_delegate> delegate, bool owns_view);
    ~Platform_view();
    auto receive_parent(void* parent) -> void;
    auto resize(int32_t w, int32_t h) -> void;
    auto redraw() -> void;
    auto native_handle() -> void* { return _view; }
private:
    const bool _owns_view{true};
    std::shared_ptr<Graphics_delegate> _delegate;
    void* _view{nullptr};
};

// Factory
struct Platform_views {
    static auto make_owning(std::shared_ptr<Graphics_delegate> delegate) -> std::unique_ptr<Platform_view> {
        return std::make_unique<Platform_view>(delegate, true);
    }

#if PLATFORM_MACOS
    static auto make_autoreleasing(std::shared_ptr<Graphics_delegate> delegate) -> std::unique_ptr<Platform_view> {
        return std::make_unique<Platform_view>(delegate, false);
    }
#endif
};