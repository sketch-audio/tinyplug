#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Params {
    // This is where you enumerate your parameter ids.
    // You can use the raw values to index into arrays and vectors.
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Gain = 0,
        Num_params
    };

    // Here you declare your parameters.
    // Your parameters will be displayed in the host in the order which they are declared here. (preorder depth-first traversal)
    // Once you ship a plug-in the tree is a permanence surface, not just presentation:
    //  - never move a parameter between groups, and never change an `identifier` (breaks AUv3
    //    host documents and preset recall)
    //  - declare `au_order()` before you ship, and only ever append to it (Logic addresses AUv2
    //    automation by index into that list)
    // You can always hide a parameter by marking its policy as `hidden` or `interface`. 
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
                    .def_val = 0,
                    .max_val = 1,
                    .units = Units::Generic,
                    .knob_adapter = Adapter::Lin{}
                }
            }
        }};
    }
};
static_assert(params::Model<Params>); // Check your interface.

} // namespace tiny::models
