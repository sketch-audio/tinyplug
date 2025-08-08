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
        cutoff,
        type,
        offset,
        num_params
    };

    // This is where you enumerate your export ids.
    // Exports are values you can write from the kernel and tinyplug will pass them to your view.
    // You can use the raw values to index into arrays and vectors.
    enum class Export_id : uint32_t {
        peak_in = 0,
        peak_out,
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
                .semantics = Bool_semantics{
                    .def_val = true,
                    .knob_adapter = Knob_adapters::make_bool()
                }
            },
            Param_spec{
                .id = enum_raw(gain),
                .name = "Gain",
                .semantics = Float_semantics{
                    .min_val = 0,
                    .def_val = 1,
                    .max_val = 1,
                    .units = Units::generic,
                    .knob_adapter = Knob_adapters::make_power(3)
                }
            },
            Param_group{.name = "Advanced", .nodes = {
                Param_spec{
                    .id = enum_raw(cutoff),
                    .name = "Cutoff",
                    .semantics = Float_semantics{
                        .min_val = 20,
                        .def_val = 1000,
                        .max_val = 20000,
                        .units = Units::hertz,
                        .knob_adapter = Knob_adapters::make_tapered(0.05f, false)
                    }
                },
                Param_spec{
                    .id = enum_raw(type),
                    .name = "Type",
                    .semantics = List_semantics{
                        .labels = {"One", "Two", "Three"},
                        .def_val = 0,
                        .knob_adapter = Knob_adapters::make_list()
                    }
                },
                Param_spec{
                    .id = enum_raw(offset),
                    .name = "Offset",
                    .semantics = Int_semantics{
                        .min_val = -12,
                        .def_val = 0,
                        .max_val = 12,
                        .units = Units::generic,
                        .knob_adapter = Knob_adapters::make_discrete()
                    }
                },
            }}
        }};
    }

    // This is so tinyplug knows how to correcly process your exports.
    // For example, 
    // - peak meters want the max unconsomed value
    // - streams simply want the latest value
    // - trigs want to happen once
    static auto export_type(Export_id id) -> Export_type
    {
        using enum Export_id;
        switch (id) {
            case peak_in: return Export_type::peak;
            case peak_out: return Export_type::peak;
            default: return Export_type::stream;
        }
    }
};
static_assert(Some_param_model<Param_model>); // Check your interface.

} // namespace tiny
