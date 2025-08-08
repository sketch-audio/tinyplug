#include "dsp_kernel.h"

#include <algorithm> // std::max

namespace tiny {

auto Dsp_kernel::reset(double sample_rate, size_t /*max_frames*/) -> void
{
    _sr = sample_rate;
}

auto Dsp_kernel::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            _values[e.id] = e.value;
        },
        [this](const Ramp_param& e) {
            _values[e.id] = e.target; 
        }
    }, event);
}

auto Dsp_kernel::process(Dsp_context& context) -> void
{
    using enum Param_model::Param_id;
    using enum Param_model::Export_id;

    const auto g = _values[enum_raw(gain)];
    
    for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = g * input;
            context.obuffers[channel][frame] = output;

            // Update peak.
            auto* curr_in = &context.exports[enum_raw(peak_in)];
            auto* curr_out = &context.exports[enum_raw(peak_out)];
            *curr_in = std::max(*curr_in, std::abs(input));
            *curr_out = std::max(*curr_out, std::abs(output));
        }
    }
}

} // namespace tiny
