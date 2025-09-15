#include "window_context.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "include/core/SkColorSpace.h"

#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"

namespace tiny {

auto Window_context::setup(const Setup& setup) -> void
{
    auto view = static_cast<NSView*>(setup.native_handle);
    _view = view;

    auto device = MTLCreateSystemDefaultDevice();
    _device = device;
    _queue = [device newCommandQueue];

    auto layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _layer = layer;

    view.layer = layer;
    view.wantsLayer = YES;

    auto backendContext = GrMtlBackendContext{};
    backendContext.fDevice.retain(_device);
    backendContext.fQueue.retain(_queue);
    _context = GrDirectContexts::MakeMetal(backendContext);
}

auto Window_context::teardown() -> void
{
    if (_context) {
        _context->abandonContext();
        _context.reset();
    }

    _layer = nil;

    auto queue = static_cast<id<MTLCommandQueue>>(_queue);
    [queue release];

    auto device = static_cast<id<MTLDevice>>(_device);
    [device release];
}

auto Window_context::begin_draw() -> void
{
    auto layer = static_cast<CAMetalLayer*>(_layer);
    auto drawable = [layer nextDrawable];
    if (!drawable) return;

    auto texture_info = GrMtlTextureInfo{};
    texture_info.fTexture.retain(drawable.texture);

    auto size = layer.drawableSize;

    auto render_target = GrBackendRenderTargets::MakeMtl(size.width, size.height, texture_info);

    auto surface = SkSurfaces::WrapBackendRenderTarget(_context.get(),
                                                       render_target,
                                                       kTopLeft_GrSurfaceOrigin,
                                                       kBGRA_8888_SkColorType,
                                                       nullptr,
                                                       nullptr);

    assert(surface && "Failed to create skia surface!");
    _surface = surface;

    _drawable = CFRetain((GrMTLHandle)drawable);
}

auto Window_context::get_canvas() -> Canvas
{
    if (!_surface) return {nullptr};
    return {_surface->getCanvas()};
}

auto Window_context::end_draw() -> void
{
    if (auto direct_context = _context.get()) {
        direct_context->flush();
        direct_context->submit();
    }

    auto drawable = static_cast<id<CAMetalDrawable>>(_drawable);

    auto queue = static_cast<id<MTLCommandQueue>>(_queue);
    auto command_buffer = ([queue commandBuffer]);
    command_buffer.label = @"Present";

    [command_buffer presentDrawable:drawable];
    [command_buffer commit];

    CFRelease(_drawable); // no arc
    _drawable = nullptr;
}

auto Window_context::on_resized() -> void
{
    const auto* view = static_cast<NSView*>(_view);
    const auto scale = std::max(view.window.backingScaleFactor, 1.0);
    const auto logical_size = view.bounds.size;
    const auto real_size = CGSizeMake(logical_size.width * scale, logical_size.height * scale);

    const auto* layer = static_cast<CAMetalLayer*>(_layer);
    layer.drawableSize = real_size;
    layer.contentsScale = scale;

    _size = {
        static_cast<int32_t>(real_size.width),
        static_cast<int32_t>(real_size.height)
    };
}

} // namespace tiny