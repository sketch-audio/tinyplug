#include "plug_processor.h"

#include <algorithm> // std::max

namespace tiny {

auto Plug_processor::reset(double sample_rate) -> void
{
    // ...
}

auto Plug_processor::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            _values[e.id] = static_cast<float>(e.value);
        },
        [this](const Ramp_param& e) {
            _values[e.id] = static_cast<float>(e.target); // You might want to handle this differently.
        },
        [this](const auto&) { /* Handle other events as needed. */ }
    }, event);
}

auto Plug_processor::process(Dsp_context& context) -> void
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
