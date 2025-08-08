#pragma once

#include <array>

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

class Dsp_kernel {
public:
    // Receive the sample rate and max block size. 
    // This a good time to resize some vectors.
    auto reset(double sample_rate, size_t max_frames) -> void;

    // Receive a render event such as `Set_param`.
    // Events are interleaved with process calls so you can consider them as happening "now".
    auto handle_event(const Render_event& event) -> void;

    // This is where you can do your signal processing.
    // In the DSP context, you have:
    // - The musical context, e.g. `beat_pos` & `tempo`
    // - Pointers to the input, output, and sidechain buffers (They could be null!)
    // - The number of frames to render (This could be a small number like one!)
    // - A place to write your exports
    auto process(Dsp_context& context) -> void;

private:

    using User_params = Param_infos<Param_model>;
    static constexpr auto num_params = User_params::num_params;

    double _sr{48000};

    User_params _param_infos{};
    std::array<float, num_params> _values{_param_infos.make_plain_defaults<float>()};

};
static_assert(Some_dsp_kernel<Dsp_kernel>); // Check your interface.

} // namespace tiny