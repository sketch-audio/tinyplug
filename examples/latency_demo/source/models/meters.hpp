#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {
    
struct Meters {
    // Enumerate meter addresses.
    enum class Address : uint32_t {
        Latency_actual = 0,
        Num_meters
    };

    // Return the spec for a meter address.
    static auto make_spec(Address address) -> meters::Spec
    {
        using namespace meters;
        switch (address) {
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