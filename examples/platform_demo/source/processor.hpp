#pragma once

#include <array>

#include <tinyplug/tinyplug.hpp>
#include "models/params.hpp"

namespace tiny::process {

class Processor {
public:
    // Receive the sample rate and the parameter values to come up holding.
    // This a good time to resize some vectors.
    auto configure(const Config& config) -> void;

    // Block-boundary commands: `Restart` says the input stream is discontinuous and hands over the
    // exact parameter values (come out as `configure` would leave you); `Latency` hands over a
    // latency the host accepted. Never allocates.
    auto reset(const Reset::Any& reset) -> void;

    // Receive a render event such as `Event::Set`.
    // Events are interleaved with process calls so you can consider them as happening "now".
    auto handle(const Event::Param& event) -> void;

    // This is where you can do your signal processing.
    // In the DSP context, you have:
    // - The host timeline, e.g. `beat_pos` & `tempo` (optional: the host may not say)
    // - Pointers to the input, output, and sidechain buffers (They could be null!)
    // - The number of frames to render (It is the plug-in's responsibility to handle any value here.)
    // - A place to write your exports
    // - The option to propose a latency change
    auto process(Context& context) -> void;

    // The framework will check and report this to the host right after calling `configure`.
    auto latency_samps() const -> uint32_t { return 0; }

    // You can get an infinite tail by returning `std::numeric_limits<uint32_t>::max()`.
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using Address = models::Params::Address;
    static constexpr auto num_params = User_params::num_params;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

    auto _adopt(std::span<const double> params) -> void; // What `configure` and `Restart` share.

};
static_assert(Interface<Processor>); // Check your interface.

} // namespace tiny::process