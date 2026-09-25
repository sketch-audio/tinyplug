// Editor support for the fake hosts (macOS): a real window for an editor to live in, and synthetic
// mouse input posted through it the way AppKit delivers a user's. Pointers are NSView* / NSWindow*,
// opaque so C++ hosts can use them.
#pragma once

#include "support.hpp"

namespace tiny::hosts {

// NSApplication, as an accessory: no Dock icon.
auto gui_init() -> void;

// An on-screen window at zero alpha: views draw and take events, and nothing flashes up.
class Window {
public:
    explicit Window(double width = 900, double height = 700);
    ~Window();
    Window(const Window&) = delete;
    auto operator=(const Window&) -> Window& = delete;

    auto content() const -> void*;       // The NSView an editor attaches to.
    auto editor_view() const -> void*;   // The content view's newest subview: the editor, once attached.
    auto add(void* view) -> void;
    auto remove(void* view) -> void;

private:
    void* _window{};
};

// Random clicks, drags, double and right clicks, moves and scrolls over `view`, each through its window.
auto send_input(void* view, Random& random, int events) -> void;

// AUv2: the unit's Cocoa view (kAudioUnitProperty_CocoaUI), retained; release with release_view.
auto make_au_cocoa_view(void* audio_unit) -> void*;
auto release_view(void* view) -> void;

} // namespace tiny::hosts
