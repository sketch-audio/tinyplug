#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {

struct Param_model {
    // This is where you enumerate your parameter ids.
    // You can use the raw values to index into arrays and vectors.
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Param_id : uint32_t {
        latency_mode = 0,
        num_params
    };

    // This is where you enumerate your export ids.
    // Exports are values you can write from the kernel and tinyplug will pass them to your view.
    // You can use the raw values to index into arrays and vectors.
    enum class Export_id : uint32_t {
        latency_actual = 0,
        num_exports
    };

    // Here you declare your parameters.
    // Your parameters will be displayed in the host in the order which they are declared here. (preorder depth-first traversal)
    // Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
    // You can always hide a parameter by marking its policy as `state` or `interface`. 
    static auto build_tree() -> Param_node
    {
        using enum Param_id;
        return Param_group{.nodes = {
            Param_spec{
                .id = enum_raw(latency_mode),
                .name = "Latency",
                .semantics = List_semantics{{"Low", "High"}},
                .policy = Host_policy::control // No automation.
            },
        }};
    }

    // This is so the framework knows how to correctly process your exports.
    // For example: 
    // - Peak meters want the max unconsumed value.
    // - Streams simply want the latest value.
    // - Trigs want to happen exactly once.
    static auto export_type(Export_id /*id*/) -> Export_type
    {
        return Export_type::stream; // You could switch on `id` here.
    }
};
static_assert(Some_param_model<Param_model>); // Check your interface.

} // namespace tiny
