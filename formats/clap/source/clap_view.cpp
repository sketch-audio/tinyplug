#include "clap_view.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

namespace tiny {

auto Clap_view::on_create() noexcept -> void
{
    // Set up the delegate callbacks.
    auto delegate = std::make_shared<View_delegate>(
        Plug_editor::preferred_size(),
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    // Create the platform view and notify the editor.
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();

    _deps.editor->on_gui_create();
}

auto Clap_view::on_show() noexcept -> void
{
    _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _deps.editor->on_gui_show({
        .actions = _actions.actor(),
        .undo_redo = _undo_history.actor(),
    });
}

auto Clap_view::on_hide() noexcept -> void
{
    _deps.editor->on_gui_hide();
    _platform_view->on_hide();
}

auto Clap_view::on_destroy() noexcept -> void
{
    _deps.editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;
}

auto Clap_view::get_size(uint32_t* w, uint32_t* h) noexcept -> void
{
    const auto platform_size = _platform_view ? _platform_view->get_size() : Plug_editor::preferred_size();
    *w = static_cast<uint32_t>(platform_size.w);
    *h = static_cast<uint32_t>(platform_size.h);
}

auto Clap_view::set_size(uint32_t w, uint32_t h) noexcept -> bool
{
    if (!_platform_view) return false;
    _platform_view->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    return true;
}

auto Clap_view::set_parent(const clap_window* window) noexcept -> bool
{
    if (!window || !_platform_view) return false;

    // Resolve the platform window type.
    const auto is_mac = Platform::resolved == Platform::Type::macos;
    auto* platform_window = is_mac ? window->cocoa : window->win32;
    _platform_view->receive_parent(platform_window);
    
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_knob_value(static_cast<uint32_t>(i)); }
    );

    return true;
}

// MARK: - private

auto Clap_view::on_draw(View_context& view_context) -> void
{
    view_impl::run_frame(
        User_meters::meter_specs(), _deps.receiver, _ui_params, _ui_meters, view_context, _deps.editor, _actions, _undo_history, *_deps.tasks
    );
}

auto Clap_view::on_notify(const Ui_notification& notification) -> void
{
    _deps.editor->on_gui_notify(notification);
}

} // namespace tiny
