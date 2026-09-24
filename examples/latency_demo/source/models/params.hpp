#pragma once

#include <array>

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Mode = 0,
        Click,
        Num_params
    };

    // What each mode delays by. Three, so a quick A-B-C shows a proposal superseding another.
    static constexpr auto mode_ms = std::array{0., 2., 20.};

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;
        return Group{.nodes = {
            Spec{
                .identity = {.address = enum_raw(Mode), .identifier = "mode"},
                .name = "Latency",
                .semantics = Semantics::List{{"0 ms", "2 ms", "20 ms"}},
                .policy = Policy::Control // Latency is structural: no automation.
            },
            Spec{
                // Replace the input with a click on every second of the timeline: with delay
                // compensation working, a bounce puts each click exactly on the second.
                .identity = {.address = enum_raw(Click), .identifier = "click"},
                .name = "Click",
                .semantics = Semantics::Bool{.def_val = false},
                .policy = Policy::Control
            },
        }};
    }

    static auto au_order() -> std::vector<Address>
    {
        using enum Address;
        return {Mode, Click};
    }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
