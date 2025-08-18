#include "dsp_kernel.h"

#include <algorithm> // std::max

namespace tiny {

auto Dsp_kernel::reset(double sample_rate) -> void
{
    _sr = sample_rate;
}

auto Dsp_kernel::handle_event(const Render_event& event) -> void
{
    using enum Param_model::Param_id;

    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            // Identify that we want a latency change.
            if (e.id == enum_raw(latency_mode) && e.value != _values[e.id]) {
                _wants_latency_change = true;
            }

            _values[e.id] = e.value;
        },
        [this](const Ramp_param& e) {
            _values[e.id] = e.target; // You might want to handle this differently.
        },
        [this](const Accepted_latency& e) {
            _latency = e.samples;
        }
    }, event);
}

auto Dsp_kernel::process(Dsp_context& context) -> void
{
    using enum Param_model::Param_id;
    using enum Param_model::Export_id;

    // Here you could propose a latency change. You shouldn't do this very often.
    // You should wait until the host accepts your latency before applying the changes.
    if (_wants_latency_change) {
        context.propose_latency = _latency == 0 ? 1000 : 0;
        _wants_latency_change = false;
    }

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

    // Export `latency_actual` so we can see if there are discrepancies in the UI.
    context.exports[enum_raw(latency_actual)] = double{_latency == 0 ? double{0} : double {1}};
}

} // namespace tiny
