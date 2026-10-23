#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// What the specimen draws with: automate the weight and the variable font follows live.
struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Weight = 0,
        Size,
        Sweep,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .identity = {.address = enum_raw(Weight), .identifier = "weight"},
                .name = "Weight",
                .semantics = Semantics::Real{.min_val = 100, .def_val = 400, .max_val = 900, .display = Display::Default{}, .knob_adapter = Adapter::Lin{}}
            },
            Spec{
                .identity = {.address = enum_raw(Size), .identifier = "size"},
                .name = "Size",
                .semantics = Semantics::Real{.min_val = 9, .def_val = 32, .max_val = 72, .display = Display::Default{}, .knob_adapter = Adapter::Lin{}}
            },
            Spec{
                .identity = {.address = enum_raw(Sweep), .identifier = "sweep"},
                .name = "Sweep",
                .semantics = Semantics::Bool{.def_val = false}
            },
        }};
    }

    static auto au_order() -> std::vector<Address> { return {Address::Weight, Address::Size, Address::Sweep}; }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
