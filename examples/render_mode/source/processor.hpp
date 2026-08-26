#pragma once

#include <array>
#include <cstdint>

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

// Demonstrates offline (bounce) detection. In realtime the output is a sine
// tone plus white noise; when the host renders offline the noise is dropped,
// leaving a clean sine — simulating a "high quality when bouncing" code path.
// The editor reads the `Offline` meter and paints the whole surface green
// (realtime) or yellow (offline).
class Processor {
public:
    auto configure(const Config& config) -> void;

    // Forget render history (host seek, bounce, un-bypass). Never allocates.
    auto clear() -> void {}

    // Manifest deferred param changes now, with no glide.
    auto snap() -> void {}
    auto handle_event(const Render_event& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using User_params = params::Infos<models::Params>;
    using Param = models::Params::Address;
    using Meter = models::Meters::Address;
    static constexpr auto num_params = User_params::num_params;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

    static constexpr double tone_hz = 440.0;

    double _sample_rate{44100};
    double _phase{0};         // radians
    double _phase_inc{0};     // radians per sample
    uint32_t _rng{0x1234567u}; // xorshift state for white noise
};
static_assert(Some_plug_processor<Processor>); // Check your interface.

} // namespace tiny::plugin
