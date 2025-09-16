#pragma once

#include <atomic>
#include <functional>
#include <utility>

#include "../tinyplug/tinyplug.h"

namespace tiny {

class Window_context; // Keep it out of the headers.

class View_delegate {
public:

    View_delegate(Rect_size initial_size, Draw_callback callback);
    ~View_delegate();
    auto set_context(std::unique_ptr<Window_context> context) -> void;
    auto draw(const User_interaction& interaction, const Time_point& time_now, bool dark_mode) -> void;
    auto on_resize(const Rect_size& size) -> void;
    auto get_size() const -> Rect_size;

private:

    Rect_size _size{};
    double _scale{1};
    Draw_callback _callback{};

    std::unique_ptr<Window_context> _context{nullptr};
    std::atomic<bool> _do_resize{false};

    auto resize_context() -> void;

};

} // namespace tiny
