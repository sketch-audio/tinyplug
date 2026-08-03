#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Params {
    enum class Address : uint32_t {
        Gain = 0,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .identity = {.address = enum_raw(Gain), .identifier = "gain"},
                .name = "Gain",
                .semantics = Semantics::Real{
                    .min_val = 0,
                    .def_val = 1,
                    .max_val = 1,
                    .units = Units::Generic,
                    .knob_adapter = Adapter::Lin{}
                }
            }
        }};
    }
};
static_assert(params::Model<Params>);

} // namespace tiny::models
