#pragma once

#include <algorithm> // std::max

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

struct Dsp_kernel {

    Dsp_kernel() {
        // Initialize dsp values with defaults.
        const auto params = _params.kernel_specs();
        for (size_t i = 0; i < params.size(); ++i) {
            const auto& param = params[i];
            _values[i] = get_plain_default(param);
        }
    }

    //
    auto reset(double /*sample_rate*/, size_t /*max_frames*/) -> void
    {
        
    }

    // The framework will make sure event calls are correctly interleaved with process calls.
    auto handle_event(const Render_event& event) -> void
    {
        std::visit(
            Inline_visitor{
                [this](const Set_param& e) {
                    _values[e.id] = e.value;
                },
                [this](const Ramp_param& e) {
                    _values[e.id] = e.target; 
                }
            }
        , event);
    }

    //
    auto process(Dsp_context& context) -> void
    {
        const auto gain = _values[enum_raw(Param_id::gain)];
        
        for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
            for (size_t frame = 0; frame < context.num_frames; ++frame) {
                const auto input = context.ibuffers[channel][frame];
                const auto output = gain * input;
                context.obuffers[channel][frame] = output;

                // Update peak.
                auto* curr_in = &context.exports[enum_raw(Export_id::peak_in)];
                auto* curr_out = &context.exports[enum_raw(Export_id::peak_out)];
                *curr_in = std::max(*curr_in, std::abs(input));
                *curr_out = std::max(*curr_out, std::abs(output));
            }
        }
    }

private:

    using User_params = Param_infos<Param_model>;
    static constexpr auto num_params = User_params::num_params;

    using Param_id = Param_model::Param_id;
    using Export_id = Param_model::Export_id;

    User_params _params{};
    std::array<float, num_params> _values{};

};
static_assert(Some_dsp_kernel<Dsp_kernel>);

} // namespace tiny