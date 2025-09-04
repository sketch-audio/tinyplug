#include "dsp_kernel.h"

#include <algorithm> // std::max

namespace tiny {

auto Dsp_kernel::reset(double sample_rate) -> void
{
    _ramper.reset(sample_rate);
}

auto Dsp_kernel::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            if (e.id == enum_raw(Param_id::gain)) {
                _ramper.set_immediate(static_cast<float>(e.value));
            }
        },
        [this](const Ramp_param& e) {
            if (e.id == enum_raw(Param_id::gain)) {
                _ramper.set_ramp(static_cast<float>(e.target), e.dur_samples);
            }
        },
        [this](const auto&) { /* Handle other events as needed. */ }
    }, event);
}

auto Dsp_kernel::process(Dsp_context& context) -> void
{
    const auto id = enum_raw(Param_id::gain);
    
    for (size_t frame = 0; frame < context.num_frames; ++frame) {
        _values[id] = _ramper.process();
        const auto g = _values[id];

        for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = g; // !!!
            context.obuffers[channel][frame] = output;
        }
    }
}

} // namespace tiny
