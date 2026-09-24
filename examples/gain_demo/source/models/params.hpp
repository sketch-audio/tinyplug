#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Gain = 0,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .identity = {.address = enum_raw(Gain), .identifier = "gain"},
                .name = "Gain",
                .semantics = Semantics::Real{.min_val = 0, .def_val = 1, .max_val = 4, .units = Units::Linear_gain, .knob_adapter = Adapter::Pow{.exp = 4}} // Mute to +12 dB, unity at 0.71.
            },
        }};
    }

    static auto au_order() -> std::vector<Address> { return {Address::Gain}; }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
