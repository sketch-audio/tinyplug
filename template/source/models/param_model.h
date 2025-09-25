#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {

struct Param_model {
    // This is where you enumerate your parameter ids.
    // You can use the raw values to index into arrays and vectors.
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Param_id : uint32_t {
        gain = 0,
        num_params
    };

    // Here you declare your parameters.
    // Your parameters will be displayed in the host in the order which they are declared here. (preorder depth-first traversal)
    // Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
    // You can always hide a parameter by marking its policy as `hidden` or `interface`. 
    static auto build_tree() -> Param_node
    {
        using enum Param_id;
        return Param_group{.nodes = {
            Param_spec{
                .id = enum_raw(gain),
                .string_id = "gain",
                .name = "Gain",
                .semantics = Real_semantics{
                    .min_val = 0,
                    .def_val = 1,
                    .max_val = 1,
                    .units = Units::generic,
                    .knob_adapter = Adapt_pow{3}
                }
            }
        }};
    }
};
static_assert(Some_param_model<Param_model>); // Check your interface.

} // namespace tiny