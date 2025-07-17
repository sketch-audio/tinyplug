#pragma once

#include "pluginterfaces/base/funknown.h"

#include "tinyplug/tinyplug.h"
#include "plug_info.h"

namespace tiny::vst3 {

using Uid_arr = Plug_info::Vst3::Uid_arr;

inline auto map_to_fuid(const Uid_arr& uid) -> Steinberg::FUID
{
    return {uid[0], uid[1], uid[2], uid[3]};
}

template <typename Id>
struct Param_unit {
    Id id;
    int32_t unit_id;
};

template <typename Id>
struct Unit_info {
    int32_t unit_id;
    int32_t parent_id;
    std::string name;
};

template <typename Id>
struct Flattened_units {
    std::vector<Unit_info<Id>> units;
    std::vector<Param_unit<Id>> param_to_unit;
};

template <typename Id>
auto flatten_tree_to_units(const Param_node<Id>& root) -> Flattened_units<Id>
{
    auto result = Flattened_units<Id>{};
    int32_t next_unit_id = 0;

    const auto visit = [&](const auto& node, int32_t parent_id, const auto& self) -> std::optional<int32_t> {
        return std::visit([&](const auto& item) -> std::optional<int32_t> {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, Param_spec<Id>>) {
                // Specs are assigned to their enclosing group’s unit
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, Param_group<Id>>) {
                const int32_t this_unit_id = next_unit_id++;
                result.units.push_back(Unit_info<Id>{
                    .unit_id = this_unit_id,
                    .parent_id = parent_id,
                    .name = item.name
                });

                for (const auto& child : item.nodes) {
                    std::visit([&](const auto& child_item) {
                        using ChildT = std::decay_t<decltype(child_item)>;
                        if constexpr (std::is_same_v<ChildT, Param_spec<Id>>) {
                            result.param_to_unit.push_back(Param_unit<Id>{
                                .id = child_item.id,
                                .unit_id = this_unit_id
                            });
                        } else if constexpr (std::is_same_v<ChildT, Param_group<Id>>) {
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