#pragma once

#include <memory>

#include "graphics_delegate.h"

// AUv2
static const unsigned kAudioUnitProperty_UserPlugin = 'plug';

struct Platform_view {
    Platform_view(std::shared_ptr<Graphics_delegate>);
    ~Platform_view();
    auto receive_parent(void* parent) -> void;
    auto resize(int32_t w, int32_t h) -> void;
    auto redraw() -> void;
private:
    std::shared_ptr<Graphics_delegate> _delegate;
    void* _view{nullptr};
};

// Create the platform view.
void* CreatePlatformView(Graphics_delegate* delegate);

// Destroy the platform view.
void DestroyPlatformView(void* view);

// Resize and redraw.
void RedrawPlatformView(void* view, Graphics_delegate* delegate);

// Attach the platform view to the parent.
void AttachPlatformView(void* parent, void* view);
