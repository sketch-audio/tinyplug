#pragma once

#include <atomic>
#include <functional>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "../skia/WindowContext.h"

#include "../tinyplug/tinyplug.h"

namespace tiny {

class View_delegate {
public:

    View_delegate(Rect_size initial_size, Draw_callback callback)
        : _size{initial_size}, _callback{std::move(callback)} {}

    auto set_context(std::unique_ptr<skwindow::WindowContext> context) -> void
    {
        if (!context) return;
        _context = std::move(context);
        resize_context();
    }

    auto draw(const User_interaction& interaction, const Time_point& time_now, bool dark_mode) -> void
    {
        // Should we resize?
        if (_do_resize.exchange(false, std::memory_order_acq_rel)) {
            resize_context(); // Safe.
        }

        if (!_context) return;

        auto surface = _context->getBackbufferSurface();
        if (!surface) return;

        auto* canvas = surface->getCanvas();
        if (!canvas) return;

        // Create the view context and draw.
        auto view_context = View_context{
            .time_now = time_now,
            .interaction = interaction,
            .canvas = canvas,
            .logical_size = _size,
            .scale = _scale,
            .dark_mode = dark_mode
        };
        _callback(view_context);

        _context->submitToGpu();
        _context->swapBuffers();
    }

    auto on_resize(const Rect_size& size) -> void
    {
        if (size.w < 0 || size.h < 0) return;
        _size = size;
        _do_resize.store(true, std::memory_order_release);
    }

    auto get_size() -> Rect_size
    {
        return _size;
    }

private:

    Rect_size _size{};
    double _scale{1};
    Draw_callback _callback{};

    std::unique_ptr<skwindow::WindowContext> _context{nullptr};

    std::atomic<bool> _do_resize{false};

    auto resize_context() -> void
    {
        if (!_context) return;
        _context->resize(_size.w, _size.h);
        _scale = _context->width() / _size.w; // Update screen scale.
    }
};

} // namespace tiny
