#pragma once

#include "clap/clap.h"

#include "tinyplug/tinyplug.h"
#include "user_plug.h"

namespace tiny::clap {

template <typename Id>
auto flatten_tree_paths(const Param_node<Id>& root) -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};

    const auto visit = [&](const auto& node, const std::string& path, const auto& self) -> void {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, Param_spec<Id>>) {
                result.push_back(path);
            } else if constexpr (std::is_same_v<T, Param_group<Id>>) {
                const auto group_path = path.empty() ? std::string{item.name} : path + "/" + item.name;
                for (const auto& child : item.nodes) {
                    self(child, group_path, self);
                }
            }
        }, node);
    };

    visit(root, "", visit);
    return result;
}

}