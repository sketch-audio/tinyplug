#pragma once

#include <array>

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

class Dsp_kernel {
public:
    // Receive the sample rate.
    // This a good time to resize some vectors.
    auto reset(double sample_rate) -> void;

    // Receive a render event such as `Set_param`.
    // Events are interleaved with process calls so you can consider them as happening "now".
    auto handle_event(const Render_event& event) -> void;

    // This is where you can do your signal processing.
    // In the DSP context, you have:
    // - The musical context, e.g. `beat_pos` & `tempo`
    // - Pointers to the input, output, and sidechain buffers (They could be null!)
    // - The number of frames to render (It is the plug-in's responsibility to handle any value here.)
    // - A place to write your exports
    // - The option to propose a latency change
    auto process(Dsp_context& context) -> void;

    // The framework will check and report this to the host right after calling `reset`.
    auto latency_samps() const -> uint32_t { return 0; }

    // You can get an infinite tail by returning `std::numeric_limits<uint32_t>::max()`.
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using User_params = Param_infos<Param_model>;
    using Param_id = Param_model::Param_id;
    static constexpr auto num_params = User_params::num_params;

    User_params _param_infos{};
    std::array<float, num_params> _values{_param_infos.make_plain_defaults<float>()};

};
static_assert(Some_dsp_kernel<Dsp_kernel>); // Check your interface.

} // namespace tiny