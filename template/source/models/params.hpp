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
                    .def_val = 1,
                    .max_val = 1,
                    .units = Units::Generic,
                    .knob_adapter = Adapter::Pow{3}
                }
            }
        }};
    }

    // The AUv2 parameter list order. Logic addresses AUv2 automation by index into this list, so
    // once you ship, this list may only ever be appended to — reordering it silently remaps every
    // automation lane your users have recorded. Keeping it separate is what lets `build_tree()`
    // stay free: insert a new parameter wherever it belongs visually, then append it here.
    static auto au_order() -> std::vector<Address>
    {
        using enum Address;
        return {
            Gain,
        };
    }
};
static_assert(params::Model<Params>); // Check your interface.
static_assert(params::Au_ordered<Params>); // Check your AU order.

} // namespace tiny::models
