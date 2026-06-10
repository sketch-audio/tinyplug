#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

struct Params {
    // This is where you enumerate your parameter ids.
    // You can use the raw values to index into arrays and vectors.
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Latency_mode = 0,
        Num_params
    };

    // Here you declare your parameters.
    // Your parameters will be displayed in the host in the order which they are declared here. (preorder depth-first traversal)
    // Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
    // You can always hide a parameter by marking its policy as `hidden` or `interface`. 
    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .address = enum_raw(Latency_mode),
                .string_id = "latency",
                .name = "Latency",
                .semantics = Semantics::List{{"Low", "High"}},
                .policy = Policy::Control // No automation.
            },
        }};
    }
};
static_assert(params::Model<Params>); // Check your interface.

} // namespace tiny::models
