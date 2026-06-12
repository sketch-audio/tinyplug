#pragma once

#include <variant>
#include <vector>
#include <string>

#include "tinyplug/tinyplug.hpp"

namespace tiny {

struct Reserved {
    static constexpr auto bypass_id = int32_t{0x60000000};
};

// Build module paths for the user's parameter tree. (Presentation order.)
inline auto tree_to_clap_modules(const params::Node& root) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};

    const auto visit = [&](const params::Node& node, const std::string& path, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const params::Spec&) { result.push_back(path); },
            [&](const params::Group& group) {
                const auto group_path = path.empty() ? std::string{group.name} : path + "/" + std::string{group.name};
                for (const auto& child : group.nodes) {
                    self(child, group_path, self);
                }
            }
        }, node);
    };

    visit(root, "", visit);
    return result;
}

} // namespace tiny