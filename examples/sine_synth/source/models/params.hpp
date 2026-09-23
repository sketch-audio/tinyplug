#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Params {
    // Once you ship a plug-in you should only add ids, not rearrange or remove!
    enum class Address : uint32_t {
        Attack = 0,
        Decay,
        Sustain,
        Release,
        Vibrato,
        Level,
        Num_params
    };

    static auto build_tree() -> params::Node
    {
        using namespace params;
        using enum Address;

        const auto time = [](Address address, const char* identifier, const char* name, double def) {
            return Spec{
                .identity = {.address = enum_raw(address), .identifier = identifier},
                .name = name,
                .semantics = Semantics::Real{.min_val = 1, .def_val = def, .max_val = 4000, .units = Units::Milliseconds, .knob_adapter = Adapter::Log{}}
            };
        };

        return Group{.nodes = {
            time(Attack, "attack", "Attack", 5),
            time(Decay, "decay", "Decay", 300),
            Spec{
                .identity = {.address = enum_raw(Sustain), .identifier = "sustain"},
                .name = "Sustain",
                .semantics = Semantics::Real{.min_val = 0, .def_val = 0.6, .max_val = 1, .units = Units::Generic, .knob_adapter = Adapter::Lin{}}
            },
            time(Release, "release", "Release", 400),
            Spec{
                // How far a fully pressed note wobbles, in semitones. Pressure (aftertouch) scales it.
                .identity = {.address = enum_raw(Vibrato), .identifier = "vibrato"},
                .name = "Vibrato",
                .semantics = Semantics::Real{.min_val = 0, .def_val = 0.5, .max_val = 2, .units = Units::Generic, .knob_adapter = Adapter::Lin{}}
            },
            Spec{
                .identity = {.address = enum_raw(Level), .identifier = "level"},
                .name = "Level",
                .semantics = Semantics::Real{.min_val = 0, .def_val = 0.5, .max_val = 1, .units = Units::Generic, .knob_adapter = Adapter::Lin{}}
            },
        }};
    }

    static auto au_order() -> std::vector<Address>
    {
        using enum Address;
        return {Attack, Decay, Sustain, Release, Vibrato, Level};
    }
};
static_assert(params::Model<Params>);
static_assert(params::Au_ordered<Params>);

} // namespace tiny::models
