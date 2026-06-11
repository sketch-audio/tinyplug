#include "../window_context.hpp"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "include/core/SkColorSpace.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"

#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"

namespace tiny {

// Metal + Skia state, hidden from window_context.hpp.
struct Window_context::Impl {
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;
    void* view{};      // NSView*
    void* device{};    // id<MTLDevice>
    void* queue{};     // id<MTLCommandQueue>
    void* layer{};     // CAMetalLayer*
    GrMTLHandle drawable{}; // id<CAMetalDrawable>
};

Window_context::Window_context() : _impl{std::make_unique<Impl>()} {}
Window_context::~Window_context() = default;

auto Window_context::setup(const Setup& setup) -> void
{
    auto view = static_cast<NSView*>(setup.native_handle);
    _impl->view = view;

    auto device = MTLCreateSystemDefaultDevice();
    _impl->device = device;
    _impl->queue = [device newCommandQueue];

    auto layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _impl->layer = layer;

    view.layer = layer;
    view.wantsLayer = YES;

    auto backendContext = GrMtlBackendContext{};
    backendContext.fDevice.retain(_impl->device);
    backendContext.fQueue.retain(_impl->queue);
    _impl->context = GrDirectContexts::MakeMetal(backendContext);
}

auto Window_context::teardown() -> void
{
    if (_impl->context) {
        _impl->context->abandonContext();
        _impl->context.reset();
    }

    _impl->layer = nil;

    auto queue = static_cast<id<MTLCommandQueue>>(_impl->queue);
    [queue release];

    auto device = static_cast<id<MTLDevice>>(_impl->device);
    [device release];
}

auto Window_context::set_drawable(void* drawable) -> void
{
    _impl->drawable = CFRetain((GrMTLHandle)drawable);
}

auto Window_context::begin_draw() -> void
{
    auto layer = static_cast<CAMetalLayer*>(_impl->layer);

    const auto drawable = [&]() -> id<CAMetalDrawable> {
        // Have to make sure we don't call next drawable when using CAMetalDisplayLink.
        if (@available(macOS 14, *)) {
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

    auto size = layer.drawableSize;

    auto render_target = GrBackendRenderTargets::MakeMtl(size.width, size.height, texture_info);

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
    const auto* view = static_cast<NSView*>(_impl->view);
    const auto scale = std::max(view.window.backingScaleFactor, 1.0);
    const auto logical_size = view.bounds.size;
    const auto real_size = CGSizeMake(logical_size.width * scale, logical_size.height * scale);

    const auto* layer = static_cast<CAMetalLayer*>(_impl->layer);
    layer.drawableSize = real_size;
    layer.contentsScale = scale;

    _size = {
        static_cast<int32_t>(real_size.width),
        static_cast<int32_t>(real_size.height)
    };
}

} // namespace tiny