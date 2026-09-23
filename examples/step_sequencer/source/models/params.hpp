#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Gate = 0,
        Velocity,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                // How much of its step a note holds.
                .identity = {.address = enum_raw(Gate), .identifier = "gate"},
                .name = "Gate",
                .semantics = Semantics::Real{.min_val = 0.05, .def_val = 0.5, .max_val = 1, .units = Units::Generic, .knob_adapter = Adapter::Lin{}}
            },
            Spec{
                .identity = {.address = enum_raw(Velocity), .identifier = "velocity"},
                .name = "Velocity",
                .semantics = Semantics::Real{.min_val = 0.05, .def_val = 0.8, .max_val = 1, .units = Units::Generic, .knob_adapter = Adapter::Lin{}}
            },
        }};
    }

    static auto au_order() -> std::vector<Address>
    {
        using enum Address;
        return {Gate, Velocity};
    }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
