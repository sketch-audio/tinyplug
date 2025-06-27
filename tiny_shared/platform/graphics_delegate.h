#pragma once

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include "platform.h"

#if PLATFORM_MACOS
#include "include/utils/mac/SkCGUtils.h"
#elif PLATFORM_WINDOWS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

struct Graphics_delegate {

    struct Size { int width{};  int height{}; };

    Graphics_delegate(Size initial_size) : _size{initial_size}
    {
        const auto info = SkImageInfo::MakeN32Premul(_size.width, _size.height);
        _surface = SkSurfaces::Raster(info);
    }

    auto draw(void* platform_context) -> void
    {
        if (!platform_context) return;

        auto* canvas = _surface->getCanvas();
        canvas->clear(SK_ColorBLUE);

#if PLATFORM_MACOS
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
        HDC hdc = (HDC)platform_context;
        SkPixmap pixmap;
        if (_surface->peekPixels(&pixmap)) {
            const auto info = pixmap.info();
            BITMAPINFO bmi = { 0 };
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = info.width();
            bmi.bmiHeader.biHeight = -info.height(); // Negative for top-down bitmap
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            // Copy pixels to HDC
            StretchDIBits(
                hdc,
                0, 0, info.width(), info.height(), // Destination
                0, 0, info.width(), info.height(), // Source
                pixmap.addr(), &bmi, DIB_RGB_COLORS, SRCCOPY
            );
        }
#endif
    }

    auto onResize(const Size& size) -> void
    {
        if (size.width < 0 || size.height < 0) return;
        _size = size;
        const auto info = SkImageInfo::MakeN32Premul(_size.width, _size.height);
        _surface = SkSurfaces::Raster(info);
    }

    auto getSize() -> Size
    {
        return _size;
    }

private:

    Size _size{};
    sk_sp<SkSurface> _surface{};
    double screen_scale{1};

};
