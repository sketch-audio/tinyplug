#include "auv2_view.h"

namespace tiny {

void* Auv2_view::create_view()
{
    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); }
    );
    _platform_view = Platform_views::make_autoreleasing(delegate);

    _uiparams = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_knob_value(i); }
    );

    return _platform_view->native_handle();
}

void Auv2_view::on_draw(View_context& view_context)
{
    _executor.on_main();
    view_impl::run_frame<User_exports>(
        _receiver, _uiparams, _uiexports, view_context, _custom_view.get()
    );
}

} // namespace tiny