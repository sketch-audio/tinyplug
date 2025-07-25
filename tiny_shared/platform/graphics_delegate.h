#pragma once

#include <atomic>
#include <functional>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "../skia/WindowContext.h"

struct Graphics_delegate {

    struct Size { int32_t width{};  int32_t height{}; };
    struct Draw_context { SkCanvas* canvas{}; Size size{}; };
    using Draw_callback = std::function<void(Draw_context&)>;

    Graphics_delegate(Size initial_size, Draw_callback callback)
        : _size{initial_size}, _callback{std::move(callback)} {}

    ~Graphics_delegate() {}

    auto set_context(std::unique_ptr<skwindow::WindowContext> context) -> void
    {
        if (!context) return;
        _context = std::move(context);
        resize_context();
    }

    auto draw() -> void
    {
        if (!_context) return;

        if (_do_resize.exchange(false, std::memory_order_acq_rel)) {
            resize_context();
        }

        auto surface = _context->getBackbufferSurface();
        if (!surface) return;

        auto* canvas = surface->getCanvas();
        if (!canvas) return;

        // Resolve canvas size.
        const auto w = static_cast<int32_t>(_scale * _size.width);
        const auto h = static_cast<int32_t>(_scale * _size.height);

        auto draw_context = Draw_context{
            .canvas = canvas,
            .size = {.width = w, .height = h} // Real pixels?
        };
        _callback(draw_context); // How to optimize the draw list?

        _context->submitToGpu();
        _context->swapBuffers(); // order?
    }

    auto onResize(const Size& size) -> void
    {
        if (size.width < 0 || size.height < 0) return;
        _size = size;
        _do_resize.store(true, std::memory_order_release);
        //resize_context();
    }

    auto getSize() -> Size
    {
        return _size;
    }

private:

    Size _size{};
    double _scale{1};
    Draw_callback _callback{};

    std::unique_ptr<skwindow::WindowContext> _context{nullptr};

    std::atomic<bool> _do_resize{false};

    auto resize_context() -> void
    {
        if (!_context) return;
        _context->resize(_size.width, _size.height);
        _scale = _context->width() / _size.width; // Update screen scale.
    }
};
