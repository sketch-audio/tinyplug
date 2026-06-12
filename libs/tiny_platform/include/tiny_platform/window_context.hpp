#pragma once

#include <memory>

#include <tinyplug/tinyplug.hpp>

class SkCanvas; // Skia canvas; the platform Impl owns the surface it comes from.

namespace tiny {

// Owns the native rendering surface (Metal-backed Skia on Apple, D3D12 or a
// CPU bitmap on Windows). All platform- and Skia-specific state lives in the
// per-platform Impl (see mac_context.mm / ios_context.mm / win_context.cpp),
// so this header carries no platform conditionals and no Skia includes.
class Window_context {
public:

    struct Setup {
        void* native_handle;
    };

    struct Canvas {
        SkCanvas* skia_canvas;
    };

    Window_context();
    ~Window_context();

    auto setup(const Setup& setup) -> void;
    auto teardown() -> void;

    auto set_drawable(void* drawable) -> void; // macOS 14
    auto begin_draw() -> void;
    auto get_canvas() -> Canvas;
    auto end_draw() -> void;

    auto on_resized() -> void;

    auto real_size() const -> Rect_size { return _size; }

private:

    Rect_size _size; // Real

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
