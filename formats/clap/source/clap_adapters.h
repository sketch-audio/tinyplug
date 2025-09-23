#pragma once

#include <variant>
#include <vector>
#include <string>

#include "tinyplug/tinyplug.h"

namespace tiny {

// CLAP state header:
// - Framework code
// - Manufacturer code
// - Plug-in code
// - Num param values (floats)
// - Num key/value pairs (State_item)
static constexpr auto num_header_items = size_t{5};
using State_header = std::array<uint32_t, num_header_items>;

// Build module paths for the user's parameter tree. (Presentation order.)
inline auto tree_to_clap_modules(const Param_node& root) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};

    const auto visit = [&](const Param_node& node, const std::string& path, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const Param_spec&) { result.push_back(path); },
            [&](const Param_group& group) {
                const auto group_path = path.empty() ? std::string{group.name} : path + "/" + group.name;
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