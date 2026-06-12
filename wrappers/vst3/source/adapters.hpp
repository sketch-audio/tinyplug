#pragma once

#include <algorithm>

#include "pluginterfaces/base/funknown.h"

#include "tinyplug/tinyplug.hpp"

#include "models/meters.hpp"
#include "models/params.hpp"
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

// tinyplug uses a read-only parameter for notifying latency changes.
static constexpr auto latency_param_id = int32_t{0x60000000};
static constexpr auto bypass_param_id = int32_t{0x60000001};

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
                        .name = std::string{group.name}
                    });
                }

                for (const auto& child : group.nodes) {
                    std::visit(Inline_visitor{
                        [&](const params::Spec& spec) {
                            result.param_to_unit.push_back(Param_unit{
                                .param_id = spec.address,
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