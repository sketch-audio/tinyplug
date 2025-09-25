#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {
    
struct Meter_model {
    // Enumerate meter addresses.
    enum class Meter_address : uint32_t {
        num_meters
    };

    // Return a list of your meter specs.
    static auto make_specs() -> std::vector<Meter_spec>
    {
        return {};
    }
};
static_assert(Some_meter_model<Meter_model>);

} // namespace tiny