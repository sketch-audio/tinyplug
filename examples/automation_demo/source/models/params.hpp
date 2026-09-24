#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Value = 0,
        Steps,
        Switch,
        Dc_out,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            // One of each kind a host automates differently: continuous, stepped, on/off.
            Spec{
                .identity = {.address = enum_raw(Value), .identifier = "value"},
                .name = "Value",
                .semantics = Semantics::Real{.min_val = 0, .def_val = 0, .max_val = 1, .units = Units::Generic, .knob_adapter = Adapter::Lin{}}
            },
            Spec{
                .identity = {.address = enum_raw(Steps), .identifier = "steps"},
                .name = "Steps",
                .semantics = Semantics::Int{.min_val = 0, .def_val = 0, .max_val = 7}
            },
            Spec{
                .identity = {.address = enum_raw(Switch), .identifier = "switch"},
                .name = "Switch",
                .semantics = Semantics::Bool{.def_val = false}
            },
            Spec{
                // Output the realized Value as DC instead of passing audio through: bounce it to see the curve.
                .identity = {.address = enum_raw(Dc_out), .identifier = "dc_out"},
                .name = "DC Out",
                .semantics = Semantics::Bool{.def_val = false},
                .policy = Policy::Control
            },
        }};
    }

    static auto au_order() -> std::vector<Address>
    {
        using enum Address;
        return {Value, Steps, Switch, Dc_out};
    }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
