#pragma once

#include <memory>

#include "../tinyplug/tinyplug.h"

#include "platform.h"

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#if PLATFORM_APPLE
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"
#endif

namespace tiny {

class Window_context {
public:

    struct Setup {
        void* native_handle;
    };

    struct Canvas {
        SkCanvas* skia_canvas;
    };

    auto setup(const Setup& setup) -> void;
    auto teardown() -> void;

    auto begin_draw() -> void;
    auto get_canvas() -> Canvas;
    auto end_draw() -> void;

    auto on_resized() -> void;

    auto real_size() const -> Rect_size { return _size; }

private:

    Rect_size _size; // Real

    sk_sp<GrDirectContext> _context;
    sk_sp<SkSurface> _surface;

#if PLATFORM_APPLE 
    void* _view; // NSView* or UIView*
    void* _device; // id<MTLDevice>
    void* _queue; // id<MTLCommandQueue>
    void* _layer; // CAMetalLayer*
    GrMTLHandle _drawable; // id<CAMetalDrawable>
#endif
    
#if PLATFORM_IOS
    void* _metal_view; // MetalView* (see implementation)
#endif

};

}
