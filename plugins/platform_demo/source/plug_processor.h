#pragma once

#include <array>

#include "tinyplug/tinyplug.h"
#include "models/meter_model.h"
#include "models/param_model.h"

namespace tiny {

class Plug_processor {
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
    using Param_address = Param_model::Param_address;
    static constexpr auto num_params = User_params::num_params;

    std::array<float, num_params> _values{User_params::make_defaults<float>(Value_space::Plain)};

};
static_assert(Some_plug_processor<Plug_processor>); // Check your interface.

} // namespace tiny