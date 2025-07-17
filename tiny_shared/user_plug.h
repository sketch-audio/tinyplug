#pragma once

#include <array>
#include <format>

#include "tinyplug/tinyplug.h"

namespace tiny {

struct User_gui {};
struct User_dsp {};

struct User_plug {
    //
    static constexpr auto io = Plug_io{
        .audio_ports = {.num_inputs = 1, .num_outputs = 1},
        .midi_ports = {.num_inputs = 0, .num_outputs = 0},
    };

    using Gui = User_gui;
    using Dsp = User_dsp;
};

using namespace params;

struct Param_model {
    // These will be used as the indices into an array!
    enum class Param_id : uint32_t {
        bypass = 0,
        drive,
        out_gain,
        curve,
        wet,
        filter_cutoff,
        num_params // Don't delete this one!
    };

    // An idea.
    enum class Export_id : uint32_t {
        clip_in = 0,
        clip_out,
        num_exports
    };

    // The number of params.
    static constexpr auto num_params = size_t{utils::to_underlying(Param_id::num_params)};

    using Spec = Param_spec<Param_id>;

    static auto build_tree() -> Param_node<Param_id>
    {
        using Group = Param_group<Param_id>;
        const auto tree = Group{
            .nodes = {{
                Group{
                    .name = "Clipper",
                    .nodes = {{
                        Spec{
                            .id = Param_id::drive,
                            .name = "Clipper Drive",
                            .short_name = "Drive!",
                            .min_val = 0,
                            .max_val = 36,
                            .def_val = 0,
                            .units = Units::decibels,
                            .dep_ids = {Param_id::out_gain},
                        },
                        Spec{
                            .id = Param_id::out_gain,
                            .name = "Clipper Out Gain",
                            .short_name = "Out",
                            .min_val = -18,
                            .max_val = 18,
                            .def_val = 0,
                            .units = Units::decibels,
                        },
                        Spec{
                            .id = Param_id::curve,
                            .name = "Clipper Curve",
                            .short_name = "Curve",
                            .min_val = 0,
                            .max_val = 2,
                            .def_val = 0,
                            .discrete = true,
                            .units = Units::indexed,
                            .labels = {{"Juicy", "Tight", "Hardest"}},
                            .knob_adapter = Knob_adapters<Param_id>::make_discrete()
                        },
                        Spec{
                            .id = Param_id::filter_cutoff,
                            .name = "Filter Cutoff",
                            .short_name = "Cutoff",
                            .min_val = 20,
                            .max_val = 20000,
                            .def_val = 1000,
                            .units = Units::hertz,
                            .knob_adapter = Knob_adapters<Param_id>::make_tapered(0.05f, false)
                        }
                    }}
                },
                Spec{
                    .id = Param_id::bypass,
                    .name = "Global Enabled",
                    .short_name = "Enabled",
                    .def_val = 1,
                    .discrete = true,
                    .units = Units::boolean,
                    .knob_adapter = Knob_adapters<Param_id>::make_discrete()
                },
                Spec{
                    .id = Param_id::wet,
                    .name = "Global Mix",
                    .short_name = "Mix",
                    .max_val = 100,
                    .def_val = 100,
                    .units = Units::percent,
                }
            }}
        };

        return std::move(tree);
    }

    using Param_values = std::array<float, num_params>;

    // 
    static auto format_string(float value, const Spec& param, const Param_values& /*context*/, bool include_units = true) -> std::string
    {
        using enum Units;
        switch (param.units) {
            case generic:
                return std::format("{:.{}f}", value, 2);
            case boolean:
                return value > 0 ? std::string{"Yes"} : std::string{"No"};
            case indexed: {
                if (const auto idx = static_cast<size_t>(value); idx < param.labels.size()) {
                    return param.labels[idx];
                }
                else {
                    return std::string{};
                }
            }
            case percent: {
                const auto suffix = std::string{include_units ? " %" : ""};
                return std::format("{:.{}f}", value, 0) + suffix;
            }
            case decibels: {
                const auto prefix = std::string{value >= 0 ? "+" : ""};
                const auto suffix = std::string{include_units ? " dB" : ""};
                return prefix + std::format("{:.{}f}", value, 1) + suffix;
            }
            case hertz: {
                // If the host doesn't want us to include units, we should just send back the plain value in Hertz.
                if (value > 1000 && include_units) {
                    const auto suffix = std::string{include_units ? " kHz" : ""};
                    return std::format("{:.{}f}", value / 1000, 1) + suffix;
                }
                else {
                    const auto suffix = std::string{include_units ? " Hz" : ""};
                    return std::format("{:.{}f}", value, 0) + suffix;
                }
            }
            default:
                return {};
        }
    }

    //
    static auto format_value(const std::string& string, const Spec& param) -> double 
    {
        char* end = nullptr;
        errno = 0;
        const auto result = std::strtod(string.c_str(), &end);

        // Check for conversion success
        if (end != string.c_str() && *end == '\0' && errno == 0) {
            return std::clamp(result, param.min_val, param.max_val);
        }

        return param.def_val;
    }
};
static_assert(Is_param_model<Param_model>);

} // namespace tiny
