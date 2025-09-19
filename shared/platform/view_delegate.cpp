#include "view_delegate.h"

#include "window_context.h"

namespace tiny {

View_delegate::View_delegate(Rect_size initial_size, Draw_callback callback, Notify_callback notify)
    : _size{initial_size}, _draw{std::move(callback)}, _notify{std::move(notify)}
{}

View_delegate::~View_delegate() 
{
    this->destroy_context();
}

auto View_delegate::assign_context(std::unique_ptr<Window_context> context) -> void
{
    this->destroy_context();
    _context = std::move(context);
    this->resize_context();
}

auto View_delegate::draw(const User_interaction& interaction, const Time_point& time_now) -> void
{
    if (!_context) return;
    _context->begin_draw();
    [[maybe_unused]] auto on_exit = Deferred{[this]() { _context->end_draw(); }};

    auto canvas = _context->get_canvas();
    if (!canvas.skia_canvas) return;

    auto view_context = View_context{
        .time_now = time_now,
        .interaction = interaction,
        .canvas = canvas.skia_canvas,
        .logical_size = _size,
        .scale = _scale
    };
    _draw(view_context);
}

auto View_delegate::notify(const Ui_notification& notification) -> void
{
    _notify(notification);
}

auto View_delegate::invalidate_context() -> void
{
    this->destroy_context();
}

auto View_delegate::on_resize(const Rect_size& size) -> void
{
    if (size.w < 0 || size.h < 0) return;
    _size = size;
    this->resize_context();
}

auto View_delegate::get_size() const -> Rect_size
{
    return _size;
}

// MARK: - private

auto View_delegate::resize_context() -> void
{
    if (!_context) return;
    _context->on_resized();
    const auto real_size = _context->real_size();
    _scale = static_cast<double>(real_size.w) / _size.w;
}

auto View_delegate::destroy_context() -> void
{
    if (_context) {
        _context->teardown();
        _context = nullptr;
    }
}

} // namespace tiny