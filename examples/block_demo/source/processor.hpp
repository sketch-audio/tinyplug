#pragma once

#include <array>
#include <complex>

#include <tinyplug/tinyplug.hpp>
#include "dsp/fft.hpp"

namespace tiny::process {

// A gain whose output is analysed two ways and published as blocks: a magnitude
// spectrum every FFT hop, and a scope sweep every rising edge.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any& reset) -> void;
    auto handle(const Event::Any& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;
    static constexpr auto num_params = User_params::num_params;

    static constexpr auto fft_size = std::size_t{2048};
    static constexpr auto hop = fft_size / 2;
    static constexpr auto sweep_size = models::Scope_frame::max_samples;
    static_assert(fft_size / 2 == models::Spectrum_frame::max_bins);

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};
    float _sr{48000.f};

    // Spectrum.
    Fft<fft_size> _fft{};
    std::array<float, fft_size> _window{};
    float _window_gain{1.f};
    std::array<float, fft_size> _history{};
    std::array<std::complex<float>, fft_size> _work{};
    std::size_t _write{};
    std::size_t _since_hop{};

    // Scope. Captured privately and copied on completion: a sweep that finishes mid-block
    // must not be overwritten by the next one before the block ends and the frame is sent.
    std::array<float, sweep_size> _sweep{};
    std::size_t _sweep_len{};
    std::size_t _waited{};
    float _previous{};
    bool _capturing{};
    bool _triggered{};

    auto _clear() -> void;
    auto _analyze(Dsp_context& context) -> void;
    auto _scope(Dsp_context& context, float x) -> void;

};
static_assert(Interface<Processor>);

} // namespace tiny::process
