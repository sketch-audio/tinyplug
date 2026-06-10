#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Meters {
    enum class Address : uint32_t {
        num_meters
    };

    static auto make_specs() -> std::vector<Meter_spec>
    {
        return {};
    }
};
static_assert(Some_meter_model<Meters>);

} // namespace tiny::models
