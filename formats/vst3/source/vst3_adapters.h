#pragma once

#include "pluginterfaces/base/funknown.h"

#include "tinyplug/tinyplug.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_info.h"

namespace tiny {

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

inline auto tree_to_units(const Param_node& root) -> Flattened_units
{
    auto result = Flattened_units{};
    auto next_unit_id = int32_t{};

    const auto visit = [&](const Param_node& node, int32_t parent_id, const auto& self) -> std::optional<int32_t> {
        return std::visit(Inline_visitor{
            [&](const Param_spec&) -> std::optional<int32_t> {
                // Specs are assigned to their enclosing group’s unit
                return std::nullopt;
            },
            [&](const Param_group& group) -> std::optional<int32_t> {
                const int32_t this_unit_id = next_unit_id++;

                result.units.push_back(Unit_info{
                    .unit_id = this_unit_id,
                    .parent_id = parent_id,
                    .name = group.name
                });

                for (const auto& child : group.nodes) {
                    std::visit(Inline_visitor{
                        [&](const Param_spec& spec) {
                            result.param_to_unit.push_back(Param_unit{
                                .param_id = spec.address,
                                .unit_id = this_unit_id
                            });
                        },
                        [&](const Param_group&) {
                            self(child, this_unit_id, self);
                        }
                    }, child);
                }

                return this_unit_id;
            }
        }, node);
    };

    visit(root, -1, visit);
    return result;
}


} // namespace tiny