#pragma once

#include <cmath>
#include <cstdint>

namespace tiny {

// Sample-accurate linear ramp for crossfade coefficients.
class Linear_ramp {
public:

    using X = float;

    Linear_ramp(X initial, X fade_ms = 10.f) : _fade_ms{fade_ms}, _target{initial}, _value{initial} {}

    auto reset(float sr) -> void
    {
        _sr = sr;
        _value = _target; // Jump to target.
        _inc = 0.f;
        _remaining = 0;
    }

    auto set_target(X value) -> void
    {
        const auto scale = std::ceil(_fade_ms * 0.001f * _sr);
        _target = value;
        _inc = (_target - _value) / scale;
        _remaining = static_cast<uint32_t>(scale);
    }

    auto value() const -> X
    {
        return _value;
    }

    auto is_ramping() const -> bool
    {
        return _remaining > 0;
    }

    auto process() -> X
    {
        step();
        return value();
    }

private:

    float _sr{48000};

    X _fade_ms{10};
    X _target{};
    X _value{};
    X _inc{};
    uint32_t _remaining{};

    auto step() -> void
    {
        const auto ramping = is_ramping();
        _value = ramping ? _value + _inc : _target;
        _remaining = ramping ? _remaining - 1 : 0;
    }

};

} // namespace tiny