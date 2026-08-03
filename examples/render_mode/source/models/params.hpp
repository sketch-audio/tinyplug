#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Params {
    enum class Address : uint32_t {
        Level = 0,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .identity = {.address = enum_raw(Level), .identifier = "level"},
                .name = "Level",
                .semantics = Semantics::Real{
                    .min_val = 0,
                    .def_val = 0.5,
                    .max_val = 1,
                    .units = Units::Generic,
                    .knob_adapter = Adapter::Pow{3}
                }
            }
        }};
    }
};
static_assert(params::Model<Params>); // Check your interface.

} // namespace tiny::models
