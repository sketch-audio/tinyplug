#include "dsp_kernel.h"

#include <algorithm> // std::max

namespace tiny {

auto Dsp_kernel::reset(double sample_rate) -> void
{
    _low.reset(sample_rate);
    _high.reset(sample_rate);
}

auto Dsp_kernel::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            // Identify that we want a latency change.
            if (e.id == enum_raw(Param_id::latency_mode) && e.value != _values[e.id]) {
                _wants_latency_change = true;
            }
            _values[e.id] = static_cast<float>(e.value);
        },
        [this](const Ramp_param& e) {
            _values[e.id] = static_cast<float>(e.target); // You might want to handle this differently.
        },
        [this](const Accepted_latency& e) {
            if (e.samples == _low.latency_samps()) {
                _curr = &_low;
            }
            else if (e.samples == _high.latency_samps()) {
                _curr = &_high;
            }
            else {
                assert(false && "Unexpected latency value!");
            }
        }
    }, event);
}

auto Dsp_kernel::process(Dsp_context& context) -> void
{
    // You should wait until the host accepts your latency before applying the changes.
    if (_wants_latency_change) {
        const auto propose = (_curr == &_low) ? _high.latency_samps() : _low.latency_samps();
        context.propose_latency = propose;
        _wants_latency_change = false;
    }

    for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
        const auto left = (channel == 0);
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = _curr->process(input, left);
            context.obuffers[channel][frame] = output;          
        }
    }

    // Export `latency_actual` so we can see if there are discrepancies in the UI.
    context.exports[enum_raw(Export_id::latency_actual)] = (_curr == &_low) ? double{} : double{1};
}

} // namespace tiny
