#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {
    
struct Meters {
    // Enumerate meter addresses.
    enum class Address : uint32_t {
        Latency_actual = 0,
        Num_meters
    };
    static constexpr auto num_meters = enum_raw(Address::Num_meters);

    // Return the spec for a meter address.
    static auto make_spec(std::uint32_t address) -> meters::Spec
    {
        using namespace meters;
        switch (static_cast<Address>(address)) {
            case Address::Latency_actual:
                return {
                    .range = Range{0, 1},
                    .policy = Policy::Stream
                };
            case Address::Num_meters:
            default:
                return {};
        }
    }
};
static_assert(meters::Model<Meters>);

} // namespace tiny::models