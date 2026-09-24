#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Drive = 0,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .identity = {.address = enum_raw(Drive), .identifier = "drive"},
                .name = "Drive",
                .semantics = Semantics::Real{.min_val = 1, .def_val = 3, .max_val = 20, .units = Units::Generic, .knob_adapter = Adapter::Log{}}
            },
        }};
    }

    static auto au_order() -> std::vector<Address> { return {Address::Drive}; }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
