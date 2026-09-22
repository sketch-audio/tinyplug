#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// Magnitude spectrum of the output, one frame per FFT hop.
struct Spectrum_frame {
    static constexpr auto max_bins = std::size_t{1024};

    std::array<float, max_bins> db{}; // Bin magnitudes in dBFS, 0..<used.
    std::uint32_t used{};
    std::uint32_t fft_size{};
    float sample_rate{};
};

// One triggered sweep of the output, one frame per trigger.
struct Scope_frame {
    static constexpr auto max_samples = std::size_t{512};

    std::array<float, max_samples> samples{};
    std::uint32_t used{};
    bool triggered{}; // False when the sweep free-ran because no rising edge arrived.
};

struct Blocks {
    using Types = std::variant<Spectrum_frame, Scope_frame>;

    enum class Address : std::uint32_t {
        Spectrum = 0,
        Scope,
        Num_blocks
    };
    static constexpr auto num_blocks = enum_raw(Address::Num_blocks);

    static constexpr auto make_spec(std::uint32_t address) -> blocks::Spec
    {
        switch (static_cast<Address>(address)) {
            case Address::Spectrum: return {blocks::kind_of<Spectrum_frame, Types>};
            case Address::Scope:    return {blocks::kind_of<Scope_frame, Types>};
            case Address::Num_blocks:
            default:                return {};
        }
    }
};
static_assert(blocks::Model<Blocks>);

} // namespace tiny::models
