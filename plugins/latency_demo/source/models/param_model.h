#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {

struct Param_model {
    // This is where you enumerate your parameter ids.
    // You can use the raw values to index into arrays and vectors.
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Param_address : uint32_t {
        latency_mode = 0,
        num_params
    };

    // Here you declare your parameters.
    // Your parameters will be displayed in the host in the order which they are declared here. (preorder depth-first traversal)
    // Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
    // You can always hide a parameter by marking its policy as `hidden` or `interface`. 
    static auto build_tree() -> Param_node
    {
        using enum Param_address;
        return Param_group{.nodes = {
            Param_spec{
                .address = enum_raw(latency_mode),
                .string_id = "latency",
                .name = "Latency",
                .semantics = List_semantics{{"Low", "High"}},
                .policy = Host_policy::control // No automation.
            },
        }};
    }
};
static_assert(Some_param_model<Param_model>); // Check your interface.

} // namespace tiny
