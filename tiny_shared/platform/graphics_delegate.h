#pragma once

#include <atomic>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "../skia/WindowContext.h"

struct Graphics_delegate {

    struct Size { int32_t width{};  int32_t height{}; };

    Graphics_delegate(Size initial_size) : _size{initial_size} {}

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
        const auto w = _scale * _size.width;
        const auto h = _scale * _size.height;

        // canvas->clear(...) was giving us some leaks.
        auto paint = SkPaint{};
        paint.setColor(SK_ColorBLUE);
        paint.setStyle(SkPaint::kFill_Style);
        canvas->drawRect(SkRect::MakeXYWH(0, 0, w, h), paint);

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
    std::unique_ptr<skwindow::WindowContext> _context{nullptr};

    std::atomic<bool> _do_resize{false};

    auto resize_context() -> void
    {
        if (!_context) return;
        _context->resize(_size.width, _size.height);
        _scale = _context->width() / _size.width; // Update screen scale.
    }
};
