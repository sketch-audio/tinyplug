#pragma once

#include <cstddef>
#include <cstdint>

namespace tiny {

struct Linear_ramper {

    constexpr Linear_ramper() = default;

    constexpr auto reset(double /*sr*/) -> void
    {
        _target = _value = {};
        _inc = {};
        _count = {};
    }

    constexpr auto set_ramp(float target, int32_t samples) -> void
    {
        if (samples < 0) return;
        _target = target;
        _inc = (target - _value) / samples;
        _count = samples;
    }

    constexpr auto set_immediate(float target) -> void
    {
        _target = _value = target;
        _inc = 0;
        _count = 0;
    }

    constexpr auto process() -> float
    {
        const auto step = _count > 0;
        _value = step ? _value + _inc : _target;
        _count = step ? _count - 1 : _count;
        return _value;
    }

private:

    float _target{};
    float _value{};
    float _inc{};
    int32_t _count{};

};

// Test ramper
static_assert([]() {
    auto result = true;

    const auto sr = 48000;
    auto ramper = Linear_ramper{};
    ramper.reset(sr);

    // 
    const auto x0 = 0.5f;
    ramper.set_immediate(x0);
    const auto y0 = ramper.process();
    result = (x0 == y0);

    const auto n = 64;
    auto x = 0.25f;
    auto y = float{};
    ramper.set_ramp(x, n);
    for (auto i = decltype(n){}; i < n; ++i) {
        y = ramper.process();
    }
    result &= (x == y);

    return result;
}());

} // namespace tiny