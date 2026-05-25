#pragma once

#include "tinyplug/tinyplug.h"

namespace tiny {

struct Param_model {
    enum class Param_address : uint32_t {
        gain = 0,
        num_params
    };

    static auto build_tree() -> Param_node
    {
        using enum Param_address;
        return Param_group{.nodes = {
            Param_spec{
                .address = enum_raw(gain),
                .string_id = "gain",
                .name = "Gain",
                .semantics = Real_semantics{
                    .min_val = 0,
                    .def_val = 1,
                    .max_val = 1,
                    .units = Units::generic,
                    .knob_adapter = Adapt_lin{}
                }
            }
        }};
    }
};
static_assert(Some_param_model<Param_model>);

} // namespace tiny
