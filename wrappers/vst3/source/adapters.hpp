#pragma once

#include <algorithm>
#include <array>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"

#include <tinyplug/tinyplug.hpp>

#include <tiny_plugin.hpp>
#include "plug_info.hpp"

namespace tiny {

// Meter value <-> normalized conversion. VST3 transports meters as normalized
// read-only output parameters, so it is the only format that needs these.
inline auto plain_to_norm(double value, const meters::Range& range) -> double
{
    const auto norm = (value - range.min_val) / (range.max_val - range.min_val);
    return std::clamp(norm, 0., 1.);
}

inline auto norm_to_plain(double value, const meters::Range& range) -> double
{
    const auto norm = std::clamp(value, 0., 1.);
    return norm * (range.max_val - range.min_val) + range.min_val;
}

// In VST3, exports are implemented as read-only parameters.
static constexpr auto export_param_offset = int32_t{0x40000000};

// Read-only parameter to notify latency change from processor -> controller (no longer used).
// static constexpr auto latency_param_id = int32_t{0x60000000};
static constexpr auto bypass_param_id = int32_t{0x60000001};

// Player controls, which VST3 hosts deliver only as parameters mapped through `IMidiMapping`:
// per channel, bend, channel pressure, then each `Control::Pedal::Kind`. Hidden, never
// automatable, never saved, and turned back into `Control` events in `process`.
static constexpr auto control_param_offset = int32_t{0x50000000};
static constexpr auto controls_per_channel = int32_t{9};
static constexpr auto num_control_params = 16 * controls_per_channel;

// With `expression`, CC 74 per channel too: MPE's third dimension, for hosts that send MPE as MIDI.
static constexpr auto timbre_param_offset = control_param_offset + num_control_params;
static constexpr auto num_timbre_params = int32_t{TINY_HAS_NOTE_EXPRESSION ? 16 : 0};

// VST3 has no pressure expression type (poly pressure is an event), so MPE pressure needs one.
static constexpr auto pressure_expression_id = Steinberg::Vst::NoteExpressionTypeID{Steinberg::Vst::kCustomStart};

// The expressions declared with `expression`, in `INoteExpressionController` order: MPE's three.
struct Expression_decl {
    Steinberg::Vst::NoteExpressionTypeID id{};
    const char* title{};
    const char* short_title{};
    double neutral{}; // Normalized.
    bool bipolar{};
};
static constexpr auto declared_expressions = std::array<Expression_decl, 3>{{
    {Steinberg::Vst::kTuningTypeID, "Tuning", "Tune", 0.5, true},
    {Steinberg::Vst::kBrightnessTypeID, "Brightness", "Brt", 0.5, true},
    {pressure_expression_id, "Pressure", "Prs", 0., false},
}};

using Uid_arr = Plug_info::Vst3::Uid_arr;

inline auto map_to_fuid(const Uid_arr& uid) -> Steinberg::FUID
{
    return {uid[0], uid[1], uid[2], uid[3]};
}

// MARK: - units

struct Param_unit {
    uint32_t param_id;
    int32_t unit_id;
};

struct Unit_info {
    int32_t unit_id;
    int32_t parent_id;
    std::string name;
};

struct Flattened_units {
    std::vector<Unit_info> units;
    std::vector<Param_unit> param_to_unit;
};

inline auto tree_to_units(const params::Node& root) -> Flattened_units
{
    auto result = Flattened_units{};
    auto next_unit_id = int32_t{1};

    const auto visit = [&](const params::Node& node, int32_t parent_id, const auto& self) -> std::optional<int32_t> {
        return std::visit(Inline_visitor{
            [&](const params::Spec&) -> std::optional<int32_t> {
                // Specs are assigned to their enclosing group’s unit
                return std::nullopt;
            },
            [&](const params::Group& group) -> std::optional<int32_t> {
                // Groups without a name are transparent wrappers — don't create a unit,
                // just pass the current parent down to children.
                const int32_t this_unit_id = group.name.empty() ? parent_id : next_unit_id++;

                if (!group.name.empty()) {
                    result.units.push_back(Unit_info{
                        .unit_id = this_unit_id,
                        .parent_id = parent_id,
                        .name = group.name
                    });
                }

                for (const auto& child : group.nodes) {
                    std::visit(Inline_visitor{
                        [&](const params::Spec& spec) {
                            result.param_to_unit.push_back(Param_unit{
                                .param_id = spec.identity.address,
                                .unit_id = this_unit_id
                            });
                        },
                        [&](const params::Group&) {
                            self(child, this_unit_id, self);
                        }
                    }, child);
                }

                return this_unit_id;
            }
        }, node);
    };

    visit(root, 0, visit);
    return result;
}


} // namespace tiny