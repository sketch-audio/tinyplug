#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {

struct Param_model {
    // This is where you enumerate your parameter ids.
    // You can use the raw values to index into arrays and vectors.
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Param_id : uint32_t {
        enabled = 0,
        gain,
        type,
        offset,
        latency_mode,
        num_params
    };

    // This is where you enumerate your export ids.
    // Exports are values you can write from the kernel and tinyplug will pass them to your view.
    // You can use the raw values to index into arrays and vectors.
    enum class Export_id : uint32_t {
        peak_in = 0,
        peak_out,
        latency_actual,
        num_exports
    };

    // Here you declare your parameters.
    // Your parameters will be displayed in the host in the order which they are declared here. (preorder depth-first traversal)
    // Once you ship a plug-in, you can rearrange the tree, but you can't remove parameters!
    // You can always hide a parameter by marking it `hidden`.
    static auto build_tree() -> Param_node
    {
        using enum Param_id;
        return Param_group{.nodes = {
            Param_spec{
                .id = enum_raw(enabled),
                .name = "Enabled",
                .semantics = Bool_semantics{}
            },
            Param_spec{
                .id = enum_raw(gain),
                .name = "Gain",
                .semantics = Real_semantics{
                    .min_val = 0,
                    .def_val = 1,
                    .max_val = 1,
                    .units = Units::generic,
                    .knob_adapter = Adapt_pow{}
                }
            },
            Param_spec{
                .id = enum_raw(latency_mode),
                .name = "Latency",
                .semantics = List_semantics{{"Low", "High"}},
                .hidden = true
            },
            Param_group{.name = "Advanced", .nodes = {
                Param_spec{
                    .id = enum_raw(type),
                    .name = "List Demo",
                    .semantics = List_semantics{}
                },
                Param_spec{
                    .id = enum_raw(offset),
                    .name = "Int Demo",
                    .semantics = Int_semantics{
                        .min_val = -12,
                        .def_val = 0,
                        .max_val = 12,
                        .units = Units::generic,
                    }
                },
            }}
        }};
    }

    // This is so the framework knows how to correctly process your exports.
    // For example: 
    // - Peak meters want the max unconsumed value.
    // - Streams simply want the latest value.
    // - Trigs want to happen exactly once.
    static auto export_type(Export_id id) -> Export_type
    {
        using enum Export_id;
        switch (id) {
            case peak_in: return Export_type::peak;
            case peak_out: return Export_type::peak;
            case latency_actual: return Export_type::stream;
            default: return Export_type::stream;
        }
    }
};
static_assert(Some_param_model<Param_model>); // Check your interface.

} // namespace tiny
