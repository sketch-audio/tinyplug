#pragma once

#include <array>

#include "tinyplug/tinyplug.h"
#include "models/meter_model.h"
#include "models/param_model.h"

#include "dsp/latency.h"
#include "dsp/stereo.h"

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
    auto latency_samps() const -> uint32_t { return _curr->latency_samps(); }

    // You can get an infinite tail by returning `std::numeric_limits<uint32_t>::max()`.
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using User_params = Param_infos<Param_model>;
    using Param_address = Param_model::Param_address;
    using Meter_address = Meter_model::Meter_address;
    static constexpr auto num_params = User_params::num_params;

    User_params _param_infos{};
    std::array<float, num_params> _values{_param_infos.make_plain_defaults<float>()};

    using Latency = Stereo<Latency>;
    double _sr{48000};
    Latency _low{0.5f};
    Latency _high{5};
    Latency* _curr{&_low};
    bool _wants_latency_change{};

};
static_assert(Some_plug_processor<Plug_processor>); // Check your interface.

} // namespace tiny