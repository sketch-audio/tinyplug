#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>

namespace tiny {

// In-place radix-2 complex FFT. Tables are built at construction, so `forward` never allocates.
template<std::size_t N>
class Fft {
public:

    static_assert(N >= 2 && (N & (N - 1)) == 0, "FFT size must be a power of two.");

    using Complex = std::complex<float>;

    Fft()
    {
        for (auto k = std::size_t{}; k < N / 2; ++k) {
            const auto angle = -2. * std::numbers::pi * static_cast<double>(k) / static_cast<double>(N);
            _twiddles[k] = Complex{static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }

        auto bits = 0;
        while ((std::size_t{1} << bits) < N) ++bits;
        for (auto i = std::size_t{}; i < N; ++i) {
            auto r = std::size_t{};
            for (auto b = 0; b < bits; ++b) {
                r |= ((i >> b) & 1) << (bits - 1 - b);
            }
            _reversed[i] = static_cast<std::uint32_t>(r);
        }
    }

    auto forward(std::array<Complex, N>& x) const -> void
    {
        for (auto i = std::size_t{}; i < N; ++i) {
            const auto j = _reversed[i];
            if (i < j) std::swap(x[i], x[j]);
        }

        for (auto len = std::size_t{2}; len <= N; len <<= 1) {
            const auto half = len / 2;
            const auto stride = N / len;
            for (auto start = std::size_t{}; start < N; start += len) {
                for (auto k = std::size_t{}; k < half; ++k) {
                    const auto t = _twiddles[k * stride] * x[start + k + half];
                    const auto u = x[start + k];
                    x[start + k] = u + t;
                    x[start + k + half] = u - t;
                }
            }
        }
    }

private:

    std::array<Complex, N / 2> _twiddles{};
    std::array<std::uint32_t, N> _reversed{};

};

} // namespace tiny
