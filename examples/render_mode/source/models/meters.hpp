#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Meters {
    // Enumerate meter addresses.
    enum class Address : uint32_t {
        Offline = 0, // 1 while the host renders offline (bounce), else 0.
        Num_meters
    };

    // Return the spec for a meter address. Only one meter (Offline); Stream
    // so the editor always sees the latest reported render mode.
    static auto make_spec(Address) -> meters::Spec
    {
        return {.range = {.min_val = 0, .max_val = 1}, .policy = meters::Policy::Stream};
    }
};
static_assert(meters::Model<Meters>);

} // namespace tiny::models
