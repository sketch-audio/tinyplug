#include "view.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

namespace tiny::clap {

auto View::on_create() noexcept -> void
{
    // Set up the delegate callbacks.
    auto delegate = std::make_shared<View_delegate>(
        _deps.initial_size,
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    // Create the platform view and notify the editor.
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();

    _deps.editor->on_gui_create();
}

auto View::on_show() noexcept -> void
{
    _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _deps.editor->on_gui_show();
}

auto View::on_hide() noexcept -> void
{
    _deps.editor->on_gui_hide();
    _platform_view->on_hide();
}

auto View::on_destroy() noexcept -> void
{
    _deps.editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;
}

auto View::get_size(uint32_t* w, uint32_t* h) noexcept -> void
{
    const auto platform_size = _platform_view ? _platform_view->get_size() : _deps.initial_size;
    *w = static_cast<uint32_t>(platform_size.w);
    *h = static_cast<uint32_t>(platform_size.h);
}

auto View::set_size(uint32_t w, uint32_t h) noexcept -> bool
{
    if (!_platform_view) return false;
    _platform_view->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    return true;
}

auto View::set_parent(const clap_window* window) noexcept -> bool
{
    if (!window || !_platform_view) return false;

    // Resolve the platform window type.
#if TINY_PLATFORM_MACOS
    auto* platform_window = window->cocoa;
#elif TINY_PLATFORM_WINDOWS
    auto* platform_window = window->win32;
#endif

    _platform_view->receive_parent(platform_window);
    
    return true;
}

// MARK: - private

auto View::on_draw(View_context& view_context) -> void
{
    using namespace params;

#if TINY_HAS_WORKER
    if (_deps.drain_worker_to_editor) _deps.drain_worker_to_editor();
#endif

    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_param(static_cast<uint32_t>(i)); }
    );
    view_impl::run_frame(
        User_meters::specs(),
        _deps.receiver,
        _ui_params,
        _ui_meters,
        view_context,
        _deps.editor,
        *_deps.actions,
        *_deps.undo_history,
        *_deps.tasks,
        [this](auto w, auto h) {
            // Editor-initiated resize (corner-drag handle → Request_resize). Ask the
            // host to resize us; it echoes back via guiSetSize, which resizes the
            // platform view and refreshes the plug-in's size cache.
            if (_deps.request_resize) {
                _deps.request_resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            }
        }
    );
}

auto View::on_notify(const Dark_mode_changed& notification) -> void
{
    _deps.editor->notify(notification);
}

} // namespace tiny::clap
