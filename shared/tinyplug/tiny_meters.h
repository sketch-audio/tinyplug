#pragma once

#include <algorithm>
#include <cassert>

#include "tiny_utils.h"

namespace tiny {

enum class Meter_policy : uint32_t {
    // Editor receives the max unconsumed value.
    peak = 0,

    // Editor receives the latest value.
    stream,

    // Editor receives a value for one frame.
    trig
};

// Linear range adapter. Some formats require us to normalize the meter values.
struct Lin_range {
    // Minimum plain value.
    double min_val{};

    // Maximum plain value.
    double max_val{1};

    // Regular.
    bool operator==(const Lin_range&) const = default;
};

// Linear map to normalized.
inline auto plain_to_norm(double value, const Lin_range& range) -> double
{
    const auto norm = (value - range.min_val) / (range.max_val - range.min_val);
    return std::clamp(norm, 0., 1.);
}

// Linear map to plain.
inline auto norm_to_plain(double value, const Lin_range& range) -> double
{
    const auto norm = std::clamp(value, 0., 1.);
    return norm * (range.max_val - range.min_val) + range.min_val;
}

// A specification for a meter.
struct Meter_spec {
    // Meter address.
    uint32_t address{};

    // Range of plain values.
    Lin_range range{};

    // How the framework should treat the meter values.
    Meter_policy policy{};
};

// Model
template<typename T>
concept Some_meter_model = requires {
    typename T::Meter_address;
    requires Enum<typename T::Meter_address>;
    requires std::same_as<std::underlying_type_t<typename T::Meter_address>, uint32_t>;
    { T::make_specs() } -> std::same_as<std::vector<Meter_spec>>;
};

// So framework can keep track of things properly.
struct Tagged_meter {
    double value{};
    bool updated{}; // Have we updated the peak/stream value this frame?
    bool trigged{}; // Have we received a trig for this frame?
};

template<Some_meter_model User_model>
class Meter_infos {
public:
    // Number of meters.
    static constexpr auto num_meters = enum_raw(User_model::Meter_address::num_meters);

    static auto meter_specs() -> const std::vector<Meter_spec>&
    {
        [[maybe_unused]] static const auto validated = [] {
            assert(specs.size() == num_meters && "Unexpected number of meter specs.");
            if (!specs.empty()) {
                [[maybe_unused]] const auto contains_all = (specs.front().address == 0) && (specs.back().address == num_meters - 1);
                [[maybe_unused]] const auto order_ascending = std::ranges::is_sorted(specs, [](const auto& a, const auto& b) {
                    return a.address < b.address;
                });
                assert(contains_all && order_ascending && "Meter specs must contain all meter addresses and be indexable by address.");
            }
            return true;
        }();

        return specs;
    }

    static auto meter_spec(uint32_t address) -> const Meter_spec&
    {
        assert(address < num_meters && "Meter address out of range.");
        return specs[address];
    }

private:
    
    inline static const std::vector<Meter_spec> specs = User_model::make_specs();

};

} // namespace tiny