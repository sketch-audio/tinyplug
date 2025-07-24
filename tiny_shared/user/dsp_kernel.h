#pragma once

#include <span>

#include "tinyplug/tinyplug.h"

#include "param_model.h"

namespace tiny {

struct Dsp_kernel {

    Dsp_kernel() {
        // Initialize dsp values with defaults.
        const auto params = _params.get_kernel_specs();
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
    auto handle_event(const Event& event) -> void
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
        const auto gain = _values[to_index(Param_id::gain)];
        
        for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
            for (size_t frame = 0; frame < context.num_frames; ++frame) {
                const auto sample = gain * context.ibuffers[channel][frame];
                context.obuffers[channel][frame] = sample;
            }
        }
    }

private:

    using User_params = Params<Param_model>;
    using Param_id = Param_model::Param_id;

    User_params _params{};
    User_params::Param_values _values{};

    auto to_index(Param_id id) -> std::underlying_type_t<Param_id>
    {
        return to_underlying(id);
    }

};
static_assert(Some_dsp_kernel<Dsp_kernel>);

}