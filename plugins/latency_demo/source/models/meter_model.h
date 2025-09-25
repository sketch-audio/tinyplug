#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {
    
struct Meter_model {
    // Enumerate meter addresses.
    enum class Meter_address : uint32_t {
        latency_actual = 0,
        num_meters
    };

    // Return a list of your meter specs.
    static auto make_specs() -> std::vector<Meter_spec>
    {
        return {
            Meter_spec{
                .address = enum_raw(Meter_address::latency_actual),
                .range = Lin_range{0, 1},
                .policy = Meter_policy::stream
            }
        };
    }
};
static_assert(Some_meter_model<Meter_model>);

} // namespace tiny