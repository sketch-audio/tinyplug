#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {
    
struct Meters {
    // Enumerate meter addresses.
    enum class Address : uint32_t {
        latency_actual = 0,
        num_meters
    };

    // Return a list of your meter specs.
    static auto make_specs() -> std::vector<Meter_spec>
    {
        return {
            Meter_spec{
                .address = enum_raw(Address::latency_actual),
                .range = Lin_range{0, 1},
                .policy = Meter_policy::stream
            }
        };
    }
};
static_assert(Some_meter_model<Meters>);

} // namespace tiny::models