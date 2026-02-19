#pragma once

#include <memory>

#include "../tinyplug/tinyplug.h"

#include "platform.h"

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/core/SkCanvas.h"

#if PLATFORM_APPLE
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"
#elif PLATFORM_WINDOWS
#define SK_DIRECT3D // tiny_deps builds Skia with D3D but we still need this define here.
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
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

    auto set_drawable(void* drawable) -> void; // macOS 14
    auto begin_draw() -> void;
    auto get_canvas() -> Canvas;
    auto end_draw() -> void;

    auto on_resized() -> void;

    auto real_size() const -> Rect_size { return _size; }

private:

    Rect_size _size; // Real

#if PLATFORM_APPLE
    sk_sp<GrDirectContext> _context;
    sk_sp<SkSurface> _surface;
    void* _view; // NSView* or UIView*
    void* _device; // id<MTLDevice>
    void* _queue; // id<MTLCommandQueue>
    void* _layer; // CAMetalLayer*
    GrMTLHandle _drawable; // id<CAMetalDrawable>
#endif
    
#if PLATFORM_IOS
    void* _metal_view; // MetalView* (see implementation)
#endif

#if PLATFORM_WINDOWS
    sk_sp<GrDirectContext> fContext;
    
    inline static constexpr int kNumFrames = 2;
    HWND fWindow;
    gr_cp<ID3D12Device> fDevice;
    gr_cp<ID3D12CommandQueue> fQueue;
    gr_cp<IDXGISwapChain3> fSwapChain;
    gr_cp<ID3D12Resource> fBuffers[kNumFrames];
    sk_sp<SkSurface> fSurfaces[kNumFrames];

    // Synchronization objects.
    unsigned int fBufferIndex;
    HANDLE fFenceEvent;
    gr_cp<ID3D12Fence> fFence;
    uint64_t fFenceValues[kNumFrames];

    auto setupSurfaces(int width, int height) -> void;
#endif
};

}
