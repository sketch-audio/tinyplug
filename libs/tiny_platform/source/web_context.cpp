// The browser's Window_context: Skia CPU raster into module memory, shown with putImageData. ImageData can't wrap
// shared memory, so the frame is copied once on the way out. Graphite on WebGPU is the later GPU path.
#include <tiny_platform/window_context.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

namespace tiny {

EM_JS(void, web_put_frame, (const char* selector, const void* pixels, int w, int h), {
    const canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) return;
    const ctx = canvas._tiny_ctx ?? (canvas._tiny_ctx = canvas.getContext("2d"));
    let image = canvas._tiny_image; // Reused while the size holds: no multi-MB garbage per frame.
    if (!image || image.width !== w || image.height !== h) image = canvas._tiny_image = new ImageData(w, h);
    image.data.set(HEAPU8.subarray(pixels, pixels + w * h * 4));
    ctx.putImageData(image, 0, 0);
});

EM_JS(double, web_device_pixel_ratio, (), { return globalThis.devicePixelRatio || 1; });

struct Window_context::Impl {
    std::string selector{};
    sk_sp<SkSurface> surface{};
};

Window_context::Window_context() : _impl(std::make_unique<Impl>()) {}
Window_context::~Window_context() = default;

auto Window_context::setup(const Setup& setup) -> void
{
    _impl->selector = static_cast<const char*>(setup.native_handle);
    this->on_resized();
}

auto Window_context::teardown() -> void { _impl->surface = nullptr; }
auto Window_context::set_drawable(void*) -> void {}
auto Window_context::begin_draw() -> void {}

auto Window_context::get_canvas() -> Canvas
{
    return {_impl->surface ? _impl->surface->getCanvas() : nullptr};
}

auto Window_context::end_draw() -> void
{
    auto pixmap = SkPixmap{};
    if (!_impl->surface || !_impl->surface->peekPixels(&pixmap)) return;
    web_put_frame(_impl->selector.c_str(), pixmap.addr(), pixmap.width(), pixmap.height());
}

// The backing store follows the canvas's CSS size at the device pixel ratio; the scale falls out of the ratio.
auto Window_context::on_resized() -> void
{
    auto css_w = 0.0, css_h = 0.0;
    emscripten_get_element_css_size(_impl->selector.c_str(), &css_w, &css_h);
    const auto ratio = web_device_pixel_ratio();
    const auto w = std::max(1, static_cast<int>(std::lround(css_w * ratio)));
    const auto h = std::max(1, static_cast<int>(std::lround(css_h * ratio)));
    emscripten_set_canvas_element_size(_impl->selector.c_str(), w, h);
    _impl->surface = SkSurfaces::Raster(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
    _size = Rect_size{w, h};
}

} // namespace tiny
