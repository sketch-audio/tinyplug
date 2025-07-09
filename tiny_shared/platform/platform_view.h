#pragma once

#include <memory>

#include "platform.h"
#include "graphics_delegate.h"

// AUv2
static const unsigned kAudioUnitProperty_UserPlugin = 'plug';

struct Platform_view {
    Platform_view(std::shared_ptr<Graphics_delegate>);
    ~Platform_view();
    auto receive_parent(void* parent) -> void;
    auto resize(int32_t w, int32_t h) -> void;
    auto redraw() -> void;
    auto native_handle() -> void* { return _view; }
private:
    std::shared_ptr<Graphics_delegate> _delegate;
    void* _view{nullptr};
};