#pragma once

#include <array>

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

class Processor {
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

    using User_params = params::Infos<models::Params>;
    using Address = models::Params::Address;
    static constexpr auto num_params = User_params::num_params;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

};
static_assert(Some_plug_processor<Processor>); // Check your interface.

} // namespace tiny::plugin