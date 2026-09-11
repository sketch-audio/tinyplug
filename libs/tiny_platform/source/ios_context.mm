#include <tiny_platform/window_context.hpp>

#include "ios_config.hpp"

#import <UIKit/UIKit.h>

#include "include/core/SkColorSpace.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

#if IOS_GRAPHICS_GPU

// MARK: - Metal backend (default)

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"

@interface MetalView : UIView
@end

@implementation MetalView
+ (Class) layerClass {
    return [CAMetalLayer class];
}
@end

namespace tiny {

// Metal + Skia state, hidden from window_context.hpp.
struct Window_context::Impl {
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;
    void* view{};       // UIView*
    void* metal_view{}; // MetalView*
    void* device{};     // id<MTLDevice>
    void* queue{};      // id<MTLCommandQueue>
    void* layer{};      // CAMetalLayer*
    GrMTLHandle drawable{}; // id<CAMetalDrawable>
};

Window_context::Window_context() : _impl{std::make_unique<Impl>()} {}
Window_context::~Window_context() = default;

auto Window_context::setup(const Setup& setup) -> void
{
    auto view = static_cast<UIView*>(setup.native_handle);
    _impl->view = view;
    
    auto frame = view.frame;
    auto metal_view = [[MetalView alloc] initWithFrame:frame];
    metal_view.multipleTouchEnabled = YES; // Don't interfere with multi-touch.
    [view addSubview: metal_view];
    _impl->metal_view = metal_view;
    
    auto device = MTLCreateSystemDefaultDevice();
    _impl->device = device;
    _impl->queue = [device newCommandQueue];

    auto layer = static_cast<CAMetalLayer*>(metal_view.layer); // Metal layer from MetalView.
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.frame = frame;
    _impl->layer = layer;

    auto backendContext = GrMtlBackendContext{};
    backendContext.fDevice.retain(_impl->device);
    backendContext.fQueue.retain(_impl->queue);
    _impl->context = GrDirectContexts::MakeMetal(backendContext);
}

auto Window_context::teardown() -> void
{
    // Don't strand a drawable on the way out.
    if (_impl->drawable) {
        CFRelease(_impl->drawable);
        _impl->drawable = nullptr;
    }

    if (_impl->context) {
        _impl->context->abandonContext();
        _impl->context.reset();
    }

    _impl->layer = nil;

    auto queue = static_cast<id<MTLCommandQueue>>(_impl->queue);
    [queue release];

    auto device = static_cast<id<MTLDevice>>(_impl->device);
    [device release];
    
    id metal_view = static_cast<id>(_impl->metal_view);
    [metal_view release];
}

auto Window_context::set_drawable(void* drawable) -> void
{
    // In case we didn't reach end draw, release the previous drawable.
    if (_impl->drawable) CFRelease(_impl->drawable);

    // A nil drawable (e.g. layer not yet renderable) must not be retained — CFRetain(nullptr) crashes.
    _impl->drawable = drawable ? CFRetain((GrMTLHandle)drawable) : nullptr;
}

auto Window_context::begin_draw() -> void
{
    auto layer = static_cast<CAMetalLayer*>(_impl->layer);

    // Make sure we start with a fresh surface.
    _impl->surface.reset();

    // Don't render into a zero-size layer.
    const auto layer_size = layer.drawableSize;
    if (layer_size.width <= 0 || layer_size.height <= 0) {
        // Release any externally-supplied drawable so end_draw can't present an undrawn frame.
        if (_impl->drawable) {
            CFRelease(_impl->drawable); // We need to handle lifetime better, but for now, just release it.
            _impl->drawable = nullptr;
        }
        return;
    }

    const auto drawable = [&]() -> id<CAMetalDrawable> {
        // Have to make sure we don't call next drawable when using CAMetalDisplayLink.
        if (@available(iOS 17, *)) {
            return static_cast<id<CAMetalDrawable>>(_impl->drawable); // We might have gotten the drawable externally.
        }
        else {
            auto nextDrawable = [layer nextDrawable];
            if (!nextDrawable) return nil;
            _impl->drawable = CFRetain((GrMTLHandle)nextDrawable);
            return nextDrawable;
        }
    }();
    if (!drawable) return;

    auto texture_info = GrMtlTextureInfo{};
    texture_info.fTexture.retain(drawable.texture);

    // Derive size from texture.
    const auto width = static_cast<int>(drawable.texture.width);
    const auto height = static_cast<int>(drawable.texture.height);

    auto render_target = GrBackendRenderTargets::MakeMtl(width, height, texture_info);

    auto surface = SkSurfaces::WrapBackendRenderTarget(_impl->context.get(),
                                                       render_target,
                                                       kTopLeft_GrSurfaceOrigin,
                                                       kBGRA_8888_SkColorType,
                                                       nullptr,
                                                       nullptr);

    assert(surface && "Failed to create skia surface!");
    _impl->surface = surface;
}

auto Window_context::get_canvas() -> Canvas
{
    if (!_impl->surface) return {nullptr};
    return {_impl->surface->getCanvas()};
}

auto Window_context::end_draw() -> void
{
    if (!_impl->drawable) return;

    if (auto direct_context = _impl->context.get()) {
        direct_context->flush();
        direct_context->submit();
    }

    auto drawable = static_cast<id<CAMetalDrawable>>(_impl->drawable);

    auto queue = static_cast<id<MTLCommandQueue>>(_impl->queue);
    auto command_buffer = ([queue commandBuffer]);
    command_buffer.label = @"Present";

    [command_buffer presentDrawable:drawable];
    [command_buffer commit];

    CFRelease(_impl->drawable); // no arc
    _impl->drawable = nullptr;
}

auto Window_context::on_resized() -> void
{
    const auto* view = static_cast<UIView*>(_impl->view);
    const auto s = view.window.screen.scale ?: [UIScreen mainScreen].scale;
    const auto scale = std::max(s, 1.0);
    const auto frame = view.frame;
    
    const auto logical_size = view.bounds.size;
    const auto real_size = CGSizeMake(logical_size.width * scale, logical_size.height * scale);
    
    const auto* metal_view = static_cast<MetalView*>(_impl->metal_view);
    metal_view.frame = frame;

    const auto* layer = static_cast<CAMetalLayer*>(_impl->layer);
    layer.frame = frame;
    layer.drawableSize = real_size;
    layer.contentsScale = scale;

    _size = {
        static_cast<int32_t>(real_size.width),
        static_cast<int32_t>(real_size.height)
    };
}

} // namespace tiny

#else

// MARK: - Raster backend
//
// Skia rasterises on the CPU into bitmaps we own and CoreAnimation composites the result:
// no Metal device, command queue, drawable pool or GPU watchdog. Kept behind the flag as
// the fallback if the Metal path proves unsafe again.

#include <algorithm>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

namespace tiny {

struct Window_context::Impl {

    // Two buffers: CoreAnimation may still be reading the image we handed it last frame,
    // so never draw into the one that is currently on screen.
    static constexpr auto num_buffers = size_t{2};

    SkBitmap bitmaps[num_buffers]{};
    sk_sp<SkSurface> surfaces[num_buffers]{};
    size_t buffer{};

    void* view{}; // UIView*
    int width{};
    int height{};

    auto resize(int w, int h) -> void
    {
        width = w;
        height = h;

        if (w <= 0 || h <= 0) {
            for (auto& surface : surfaces) surface.reset();
            for (auto& bitmap : bitmaps) bitmap.reset();
            return;
        }

        // Explicitly BGRA to match the CGBitmapInfo in `end_draw`. N32 resolves to RGBA
        // in this Skia build, which swaps red and blue.
        const auto info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        for (auto i = size_t{}; i < num_buffers; ++i) {
            bitmaps[i].allocPixels(info);
            surfaces[i] = SkSurfaces::WrapPixels(info, bitmaps[i].getPixels(), bitmaps[i].rowBytes());
        }
    }
};

Window_context::Window_context() : _impl{std::make_unique<Impl>()} {}
Window_context::~Window_context() = default;

auto Window_context::setup(const Setup& setup) -> void
{
    auto* view = static_cast<UIView*>(setup.native_handle);
    _impl->view = view;
    view.layer.contentsGravity = kCAGravityResize;

    this->on_resized();
}

auto Window_context::teardown() -> void
{
    for (auto& surface : _impl->surfaces) surface.reset();
    for (auto& bitmap : _impl->bitmaps) bitmap.reset();

    if (auto* view = static_cast<UIView*>(_impl->view)) view.layer.contents = nil;
    _impl->view = nullptr;
}

// Metal-only concept; the raster path owns its own pixels.
auto Window_context::set_drawable(void* /*drawable*/) -> void
{
}

auto Window_context::begin_draw() -> void
{
}

auto Window_context::get_canvas() -> Canvas
{
    auto& surface = _impl->surfaces[_impl->buffer];
    if (!surface) return Canvas{nullptr};

    auto* canvas = surface->getCanvas();
    canvas->resetMatrix();
    canvas->clear(SK_ColorBLACK);
    return Canvas{canvas};
}

auto Window_context::end_draw() -> void
{
    auto& bitmap = _impl->bitmaps[_impl->buffer];
    auto* view = static_cast<UIView*>(_impl->view);
    if (!view || !bitmap.getPixels() || _impl->width <= 0 || _impl->height <= 0) return;

    const auto row_bytes = bitmap.rowBytes();
    const auto length = row_bytes * static_cast<size_t>(_impl->height);

    // The provider borrows our pixels rather than copying them — hence the second buffer.
    auto provider = CGDataProviderCreateWithData(nullptr, bitmap.getPixels(), length, nullptr);
    auto space = CGColorSpaceCreateDeviceRGB();

    const auto layout = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | kCGImageAlphaPremultipliedFirst;
    auto image = CGImageCreate(static_cast<size_t>(_impl->width), static_cast<size_t>(_impl->height),
                               8, 32, row_bytes, space, layout, provider,
                               nullptr, false, kCGRenderingIntentDefault);

    view.layer.contents = (id)image;

    CGImageRelease(image);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);

    _impl->buffer = (_impl->buffer + 1) % Impl::num_buffers;
}

auto Window_context::on_resized() -> void
{
    auto* view = static_cast<UIView*>(_impl->view);
    if (!view) return;

    const auto s = view.window.screen.scale ?: [UIScreen mainScreen].scale;
    const auto scale = std::max(s, 1.0);

    const auto logical = view.bounds.size;
    const auto w = static_cast<int>(logical.width * scale);
    const auto h = static_cast<int>(logical.height * scale);

    view.layer.contentsScale = scale;
    _impl->resize(w, h);

    _size = {static_cast<int32_t>(w), static_cast<int32_t>(h)};
}

} // namespace tiny

#endif // IOS_GRAPHICS_GPU
