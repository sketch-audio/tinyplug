#pragma once

#include <vector>

namespace tiny {

struct Latency {

    constexpr Latency() = default;
    constexpr explicit Latency(double latency_ms) : _latency_ms(latency_ms) {}

    constexpr auto reset(double sr) -> void
    {
        _sr = sr;
        const auto latency_samples = this->_delay_samples();
        const auto min_samples = latency_samples + 2; // So we can have zero latency.
        auto n = size_t{1};
        while (n < static_cast<size_t>(min_samples)) n *= 2;
        _samples.assign(n, 0);
        _idx = 0;
        _msk = n - 1;
        _off = static_cast<size_t>(latency_samples);
        const auto doff = static_cast<double>(_off);
        _frac = static_cast<float>(latency_samples - doff);
        _eta = (1 - _frac) / (1 + _frac);
        _z = 0;
    }

    constexpr auto process(float x) -> float
    {
        write(x);
        return read();
    }
    
    constexpr auto latency_samps() -> uint32_t
    {
        return static_cast<uint32_t>(this->_delay_samples());
    }

private:

    // Below this the allpass is both pointless and ill-conditioned: `_eta` tends to 1 as
    // `_frac` tends to 0, which puts its pole on the unit circle at Nyquist. Snap to the
    // integer tap instead. A thousandth of a sample is ~20 ns at 48k.
    static constexpr auto min_frac = 1e-3f;

    // `ms * sr / 1000`, not `ms * 1e-3 * sr`. `1e-3f` is a float literal — 0.001000000047 —
    // so 0.5 ms at 48k came out 24.0000011 rather than 24, `_frac` was never exactly zero,
    // and the integer path above was dead code at every sample rate. `reset` and
    // `latency_samps` share this so they cannot disagree.
    constexpr auto _delay_samples() const -> double
    {
        return _latency_ms * _sr / 1000.;
    }

    double _sr{48000};
    double _latency_ms{};

    std::vector<float> _samples{};

    size_t _idx{};
    size_t _msk{};

    size_t _off{};
    float _frac{};
    
    // allpass interpolation
    float _eta = 0;
    float _z = 0;
    
    constexpr auto write(float x) -> void
    {
        _samples[_idx & _msk] = x;
        --_idx;
    }

    constexpr auto read(bool post_write = true) -> float
    {
        const auto i = post_write ? size_t{1} : size_t{0};
        return _frac < min_frac ? _read(_off + i) : _allpass(_off + i, _frac);
    }

    constexpr auto _read(size_t off) const -> float
    {
        return _samples[(_idx + off) & _msk];
    }
    
    // See: https://ccrma.stanford.edu/~jos/pasp/First_Order_Allpass_Interpolation.html
    constexpr auto _allpass(size_t off, float /*frac*/) -> float
    {
        const auto x0 = _samples[(_idx + off) & _msk];
        const auto x1 = _samples[(_idx + off + 1) & _msk];
        const auto output = x1 + _eta * (x0 - _z);
        _z = output;
        return output;
    }
    
};

// Test fixed delay
static_assert([]() {
    const auto samples = 16;
    const auto sr = 48000.f;
    const auto latency_ms = samples * 1000 / sr;
    
    auto delay = Latency{latency_ms};
    delay.reset(sr);
    
    // Write to the delay line.
    auto y = delay.process(1);
    
    // For a one-sample delay, the first call should produce an output of ~1.
    // For an n-sample delay, the nth call should produce an output of ~1.
    for (size_t i = 1; i <= samples; ++i) {
        y = delay.process(0);
    }

    auto abs_ = [](auto x) { return x < 0 ? -x : x; }; // constexpr
    return abs_(y - float{1}) < 1e-5f;
}());

} // namespace tiny