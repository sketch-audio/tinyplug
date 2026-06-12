#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace tiny {

// Integer-sample circular buffer delay line for bypass latency compensation.
class Delay_line {
public:

    using X = float;

    auto resize(size_t min_samples) -> void
    {
        auto n = size_t{1};
        while (n < min_samples) n *= 2;
        _samples.assign(n, 0.f);
        _idx = 0;
        _mask = n - 1;
    }

    auto clear() -> void
    {
        std::fill(_samples.begin(), _samples.end(), 0.f);
        _idx = 0;
    }

    auto write(X x) -> void
    {
        _samples[_idx & _mask] = x;
        --_idx;
    }

    auto read(size_t delay, bool post_write = true) -> X
    {
        const auto off = post_write ? size_t{1} : size_t{0};
        return _samples[(_idx + delay + off) & _mask];
    }

private:

    std::vector<X> _samples{};
    size_t _idx{};
    size_t _mask{};

};

} // namespace tiny