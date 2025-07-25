#pragma once

#include <functional>
#include <vector>
#include <string>

#include "tinyplug/tinyplug.h"

namespace tiny::clap {

// MARK: - modules

template <typename Id>
inline auto tree_to_clap_modules(const Param_node<Id>& root) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};

    const auto visit = [&](const Param_node<Id>& node, const std::string& path, const auto& self) -> void {
        std::visit(
            Inline_visitor{
                [&](const Param_spec<Id>&) {
                    result.push_back(path);
                },
                [&](const Param_group<Id>& group) {
                    const auto group_path = path.empty() ? std::string{group.name} : path + "/" + group.name;

                    for (const auto& child : group.nodes) {
                        self(child, group_path, self);
                    }
                }
            }
        , node);
    };

    visit(root, "", visit);
    return result;
}

} // namespace tiny