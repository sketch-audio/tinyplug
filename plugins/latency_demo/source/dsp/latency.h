#pragma once

#include <vector>

namespace tiny {

struct Latency {

    constexpr Latency() = default;
    constexpr explicit Latency(double latency_ms) : _latency_ms(latency_ms) {}

    constexpr auto reset(double sr) -> void
    {
        _sr = sr;
        const auto latency_samples = _latency_ms * 1e-3f * _sr;
        const auto min_samples = latency_samples + 2; // So we can have zero latency.
        auto n = size_t{1};
        while (n < min_samples) n *= 2;
        _samples.assign(n, 0);
        _idx = 0;
        _msk = n - 1;
        _off = static_cast<size_t>(latency_samples);
        _frac = static_cast<float>(latency_samples - _off);
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
        return static_cast<uint32_t>(_latency_ms * 1e-3f * _sr);
    }

private:
    
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
        const auto i = post_write ? 1 : 0;
        return _frac == 0 ? _read(_off + i) : _allpass(_off + i, _frac);
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