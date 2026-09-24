#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// Peak per channel, before and after the gain. Linear; the editor draws them in dB.
struct Meters {
    enum class Address : uint32_t {
        In_left = 0,
        In_right,
        Out_left,
        Out_right,
        Num_meters
    };
    static constexpr auto num_meters = enum_raw(Address::Num_meters);

    static auto make_spec(uint32_t) -> meters::Spec
    {
        return {.range = meters::Range{0, 4}, .policy = meters::Policy::Peak}; // Up to +12 dB.
    }
};
static_assert(meters::Model<Meters>);

} // namespace tiny::models
