#pragma once

#include <span>

#include "tinyplug/tinyplug.h"

#include "param_model.h"

namespace tiny {

struct Dsp_kernel {
    //
    auto reset(double /*sample_rate*/, size_t /*max_frames*/) -> void
    {
        // Initialize internal state with parameter defaults.
        auto specs = params::flatten_tree(Param_model::build_tree());
        params::sort_param_specs_by_id(specs);

        for (size_t i = 0; i < specs.size(); ++i) {
            const auto& spec = specs[i];
            _params[i] = params::get_plain_default(spec);
        }
    }

    // The framework will make sure event calls are correctly interleaved with process calls.
    auto handle_event(const Event& event) -> void
    {
        std::visit(Inline_visitor{
            [this](const Set_param& e) {
                Values::set(_params, e.id, e.value);
            },
            [this](const Ramp_param& e) {
                Values::set(_params, e.id, e.target);
            }
        }, event);
    }

    //
    auto process(Dsp_context& context) -> void
    {
        using Param_id = Param_model::Param_id;
        const auto gain = Values::get(_params, Param_id::lin_gain);

        for (size_t channel = 0; channel < 2; ++channel) {
            for (size_t frame = 0; frame < context.num_frames; ++frame) {
                const auto sample = gain * context.ibuffers[channel][frame];
                context.obuffers[channel][frame] = sample;
            }
        }
    }

private:

    Param_model::Param_values _params{};

};
static_assert(Some_dsp_kernel<Dsp_kernel>);

}