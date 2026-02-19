#pragma once

#include <atomic>
#include <functional>
#include <utility>

#include "../tinyplug/tinyplug.h"

namespace tiny {

class Window_context; // Keep it out of the headers.

class View_delegate {
public:

    View_delegate(Rect_size initial_size, Draw_callback callback, Notify_callback notify);
    ~View_delegate();

    auto assign_context(std::unique_ptr<Window_context> context) -> void;

    auto set_drawable(void* drawable) -> void; // macOS 14
    auto draw(const User_interaction& interaction, const Time_point& time_now) -> void;
    auto notify(const Ui_notification&) -> void;
    auto invalidate_context() -> void;

    auto on_resize(const Rect_size& size) -> void;
    auto get_size() const -> Rect_size;

private:

    Rect_size _size{}; // Logical
    double _scale{1};
    Draw_callback _draw{};
    Notify_callback _notify{};

    std::unique_ptr<Window_context> _context{nullptr};
    
    auto resize_context() -> void;
    auto destroy_context() -> void;

};

} // namespace tiny
