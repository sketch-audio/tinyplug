#include "auv3_view.h"

namespace tiny {

void* Auv3_view::create_view()
{
    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); }
    );
    _platform_view = Platform_views::make_autoreleasing(delegate); // TODO: - revisit

    _uiparams = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_knob_value(static_cast<uint32_t>(i)); }
    );

    _custom_view->on_create(_actions.make_receiver(), _tasks.make_receiver());

    return _platform_view->native_handle();
}

void Auv3_view::on_draw(View_context& view_context)
{
    view_impl::run_frame<User_exports>(
        _receiver, _uiparams, _uiexports, view_context, _custom_view.get(), _actions, _tasks
    );
}

} // namespace tiny
