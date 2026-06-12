#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Meters {
    enum class Address : uint32_t {
        Num_meters
    };

    static auto make_spec(Address) -> meters::Spec
    {
        return {};
    }
};
static_assert(meters::Model<Meters>);

} // namespace tiny::models
