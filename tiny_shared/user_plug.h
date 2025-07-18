#pragma once

#include <array>
#include <format>
#include <optional>

#include "tinyplug/tinyplug.h"

namespace tiny {

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
                            .short_name = "Drive",
                            .semantics = Float{
                                .min_val = 0,
                                .def_val = 0,
                                .max_val = 36,
                                .units = Units::decibels,
                                .knob_adapter = Knob_adapters::make_linear()
                            }
                        },
                        Spec{
                            .id = Param_id::out_gain,
                            .name = "Clipper Out Gain",
                            .short_name = "Out",
                            .semantics = Float{
                                .min_val = -18,
                                .def_val = 0,
                                .max_val = 18,
                                .units = Units::decibels,
                                .knob_adapter = Knob_adapters::make_linear()
                            }
                        },
                        Spec{
                            .id = Param_id::curve,
                            .name = "Clipper Curve",
                            .short_name = "Curve",
                            .semantics = List{
                                .labels = {"Juicy", "Tight", "Hardest"},
                                .knob_adapter = Knob_adapters::make_list()
                            }
                        },
                        Spec{
                            .id = Param_id::filter_cutoff,
                            .name = "Filter Cutoff",
                            .short_name = "Cutoff",
                            .semantics = Float{
                                .min_val = 20,
                                .def_val = 1000,
                                .max_val = 20000,
                                .units = Units::hertz,
                                .knob_adapter = Knob_adapters::make_tapered(0.05f, false)
                            }
                        }
                    }}
                },
                Spec{
                    .id = Param_id::bypass,
                    .name = "Global Enabled",
                    .short_name = "Enabled",
                    .semantics = Bool{
                        .def_val = true,
                        .knob_adapter = Knob_adapters::make_bool()
                    },
                },
                Spec{
                    .id = Param_id::wet,
                    .name = "Global Mix",
                    .short_name = "Mix",
                    .semantics = Float{
                        .min_val = 0,
                        .def_val = 100,
                        .max_val = 100,
                        .units = Units::percent,
                        .knob_adapter = Knob_adapters::make_linear()
                    }
                }
            }}
        };

        return std::move(tree);
    }

    using Param_values = std::array<float, num_params>;

    // 
    static auto format_string(double host_value, const Spec& param, const Param_values& /*context*/, bool include_units = true) -> std::string
    {
        return std::visit(Inline_visitor{
            [&](const Bool&) { return host_value > 0.5f ? std::string{"True"} : std::string{"False"}; },
            [&](const List& l) {
                const auto idx = static_cast<size_t>(host_value);
                return std::string{l.labels[idx]};
            },
            [&](const Float& f) {
                using enum Units;
                const auto value = f.knob_adapter.norm_to_plain(f, host_value);
                switch (f.units) {
                    case generic:
                        return std::format("{:.{}f}", value, 2);
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
                        return std::string{};
                }
            }
        }, param.semantics);
    }

    
    static auto format_value(const std::string& string, const Spec& /*param*/) -> std::optional<double>
    {
        char* end = nullptr;
        errno = 0;
        const auto result = std::strtod(string.c_str(), &end);

        // Check for conversion success
        if (end != string.c_str() && *end == '\0' && errno == 0) {
            return result;
        }

        return std::nullopt;
    }
};
static_assert(Is_param_model<Param_model>);

} // namespace tiny
