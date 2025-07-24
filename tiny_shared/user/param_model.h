#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {

struct Param_model {
    // These will be used as the indices into an array!
    enum class Param_id : uint32_t {
        enabled = 0,
        gain,
        cutoff,
        type,
        offset,
        num_params
    };

    // These will be used as the indices into an array!
    enum class Export_id : uint32_t {
        peak_in = 0,
        peak_out,
        num_exports
    };

    // Declare your parameter info here.
    static auto build_tree() -> Param_node<Param_id>
    {
        using Group = Param_group<Param_id>;
        using Spec = Param_spec<Param_id>;
        
        return Group{
            .nodes = {
                Spec{
                    .id = Param_id::enabled,
                    .name = "Enabled",
                    .semantics = Bool_semantics{
                        .def_val = true,
                        .knob_adapter = Knob_adapters::make_bool()
                    }
                },
                Spec{
                    .id = Param_id::gain,
                    .name = "Gain",
                    .semantics = Float_semantics{
                        .min_val = 0,
                        .def_val = 1,
                        .max_val = 1,
                        .units = Units::generic,
                        .knob_adapter = Knob_adapters::make_power(3)
                    }
                },
                Group{
                    .name = "Advanced",
                    .nodes = {
                        Spec{
                            .id = Param_id::cutoff,
                            .name = "Cutoff",
                            .semantics = Float_semantics{
                                .min_val = 20,
                                .def_val = 1000,
                                .max_val = 20000,
                                .units = Units::hertz,
                                .knob_adapter = Knob_adapters::make_tapered(0.05f, false)
                            }
                        },
                        Spec{
                            .id = Param_id::type,
                            .name = "Type",
                            .semantics = List_semantics{
                                .labels = {"One", "Two", "Three"},
                                .def_val = 0,
                                .knob_adapter = Knob_adapters::make_list()
                            }
                        },
                        Spec{
                            .id = Param_id::offset,
                            .name = "Offset",
                            .semantics = Int_semantics{
                                .min_val = -12,
                                .def_val = 0,
                                .max_val = 12,
                                .units = Units::generic,
                                .knob_adapter = Knob_adapters::make_discrete()
                            }
                        },
                    }
                }
            }
        };
    }
};
static_assert(Some_param_model<Param_model>);

} // namespace tiny
