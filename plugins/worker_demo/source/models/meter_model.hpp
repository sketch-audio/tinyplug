#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny {

struct Meter_model {
    enum class Meter_address : uint32_t {
        num_meters
    };

    static auto make_specs() -> std::vector<Meter_spec>
    {
        return {};
    }
};
static_assert(Some_meter_model<Meter_model>);

} // namespace tiny
