#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>

#include "tiny_utils.hpp"

namespace tiny::meters {

// Linear range adapter. VST3 implementation requires normalization.
struct Range {
    // Minimum plain value.
    double min_val{};

    // Maximum plain value.
    double max_val{1};

    // Regular.
    auto operator==(const Range&) const -> bool = default;
};

// How the framework should treat your meter values.
enum class Policy : uint32_t {
    Peak = 0,   // Editor receives the maximum unconsumed value.
    Stream,     // Editor receives the latest value.
    Trig        // Editor receives a value for one frame.
};

// A specification for a meter.
struct Spec {
    // Range of plain values.
    Range range{};

    // How the framework should treat the meter values.
    Policy policy{};

    // Regular.
    auto operator==(const Spec&) const -> bool = default;
};

// Model
template<typename T>
concept Model = requires(typename T::Address a) {
    typename T::Address;
    requires Enum<typename T::Address>;
    requires std::same_as<std::underlying_type_t<typename T::Address>, uint32_t>;
    { T::Address::Num_meters } -> std::same_as<typename T::Address>;
    { T::make_spec(a) } -> std::same_as<Spec>;
};

template<Model User_model>
class Infos {
public:
    // Number of meters.
    static constexpr auto num_meters = enum_raw(User_model::Address::Num_meters);

    static auto specs() -> const std::array<Spec, num_meters>&
    {
        return _specs;
    }

    static auto spec(uint32_t address) -> const Spec&
    {
        assert(address < num_meters && "Meter address out of range.");
        return _specs[address];
    }

private:

    // Built by querying the model for each address; indexable by address by construction.
    inline static const std::array<Spec, num_meters> _specs = []() {
        auto arr = std::array<Spec, num_meters>{};
        for (auto i = uint32_t{}; i < num_meters; ++i) {
            const auto addr = static_cast<typename User_model::Address>(i);
            arr[i] = User_model::make_spec(addr);
        }
        return arr;
    }();

};

} // namespace tiny::meters
