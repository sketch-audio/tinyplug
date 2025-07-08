#pragma once

#include "platform.h"

#if PLATFORM_MACOS
#include "include/utils/mac/SkCGUtils.h"
#elif PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"

#include "../skia/WindowContext.h"

struct Graphics_delegate {

    struct Size { int width{};  int height{}; };

    Graphics_delegate(Size initial_size) : _size{initial_size}
    {
        const auto info = SkImageInfo::MakeN32Premul(_size.width, _size.height);
        _surface = SkSurfaces::Raster(info);
    }

    ~Graphics_delegate()
    {

    }

    auto set_context(std::unique_ptr<skwindow::WindowContext> context) -> void
    {
        _context = std::move(context);
        _context->resize(_size.width, _size.height);
    }

    auto draw(void* platform_context) -> void
    {
#if PLATFORM_MACOS
        if (!platform_context) return;
        SkPixmap pixmap;
        _surface->peekPixels(&pixmap);
        SkBitmap bmp;
        bmp.installPixels(pixmap);
        CGContext* pCGContext = (CGContextRef)platform_context;
        CGContextSaveGState(pCGContext);
        CGContextScaleCTM(pCGContext, 1 / screen_scale, 1 / screen_scale);
        SkCGDrawBitmap(pCGContext, bmp, 0, 0);
        CGContextRestoreGState(pCGContext);
#elif PLATFORM_WINDOWS
        if (!_context) return;
        auto surface = _context->getBackbufferSurface();
        if (!surface) return;
        auto* canvas = surface->getCanvas();

        // canvas->clear(...) was giving us some leaks.
        auto paint = SkPaint{};
        paint.setColor(SK_ColorBLUE);
        paint.setStyle(SkPaint::kFill_Style);
        canvas->drawRect(SkRect::MakeXYWH(0, 0, _size.width, _size.height), paint);

        _context->submitToGpu();
        _context->swapBuffers(); // order?
#endif
    }

    auto onResize(const Size& size) -> void
    {
        if (size.width < 0 || size.height < 0) return;
        _size = size;
        
        if (_context) {
            _context->resize(_size.width, _size.height);
        }
    }

    auto getSize() -> Size
    {
        return _size;
    }

private:

    Size _size{};
    std::unique_ptr<skwindow::WindowContext> _context{nullptr};

    sk_sp<SkSurface> _surface{};
    double screen_scale{1};

};
