#pragma once

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#if defined(__APPLE__)
#include "include/utils/mac/SkCGUtils.h"
#else
#endif

// AUv2
static const unsigned kAudioUnitProperty_UserPlugin = 'plug';

struct Graphics_delegate;

// Create the platform view.
void* CreatePlatformView(Graphics_delegate* delegate);

// Destroy the platform view.
void DestroyPlatformView(void* view);

void RedrawPlatformView(void* view, Graphics_delegate* delegate);

// Attach the platform view to the parent.
void AttachPlatformView(void* parent, void* view);


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

        #if defined(__APPLE__)
        SkPixmap pixmap;
        _surface->peekPixels(&pixmap);
        SkBitmap bmp;
        bmp.installPixels(pixmap);
        CGContext* pCGContext = (CGContextRef)platform_context;
        CGContextSaveGState(pCGContext);
        CGContextScaleCTM(pCGContext, 1 / screen_scale, 1 / screen_scale);
        SkCGDrawBitmap(pCGContext, bmp, 0, 0);
        CGContextRestoreGState(pCGContext);
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
