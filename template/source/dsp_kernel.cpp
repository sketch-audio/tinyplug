#include "dsp_kernel.h"

#include <algorithm> // std::max

namespace tiny {

auto Dsp_kernel::reset(double sample_rate) -> void
{
    // ...
}

auto Dsp_kernel::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            _values[e.id] = e.value;
        },
        [this](const Ramp_param& e) {
            _values[e.id] = e.target; // You might want to handle this differently.
        },
        [this](const auto&) { /* Handle other events as needed. */ }
    }, event);
}

auto Dsp_kernel::process(Dsp_context& context) -> void
{
    const auto g = _values[enum_raw(Param_id::gain)];
    
    for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = g * input;
            context.obuffers[channel][frame] = output;          
        }
    }
}

} // namespace tiny
