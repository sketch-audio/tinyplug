#pragma once

#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "AAX.h"

#include "tinyplug/tinyplug.h"

namespace tiny {

// MARK: - AAX id <--> tiny

inline auto tiny_id_to_aax(uint32_t tiny_id) -> std::optional<std::string>
{
    // Prefix: "0x", Base: 16 (with leading 0s)
    return std::format("0x{:08x}", tiny_id);
}

inline auto aax_id_to_tiny(AAX_CParamID aax_id) noexcept -> std::optional<uint32_t>
{
    // Maybe in the future we will want to use a map for this.
    static constexpr auto aax_id_prefix = "0x";
    static constexpr auto aax_id_base = 16;

    if (!aax_id) return std::nullopt;

    const auto size = std::strlen(aax_id);
    const auto poff = std::strlen(aax_id_prefix);

    auto tiny_id = uint32_t{};
    const auto [ptr, ec] = std::from_chars(
        aax_id + poff,
        aax_id + size,
        tiny_id,
        aax_id_base
    );

    if (ec != std::errc{} || ptr != aax_id + size) {
        return std::nullopt;
    }

    return tiny_id;
}

// MARK: - tree_to_aax_ids

inline auto tree_to_aax_ids(const Param_node& root) -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};

    const auto visit = [&](const Param_node& node, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const Param_spec& spec) {
                if (const auto aax_id = tiny_id_to_aax(spec.id)) {
                    result.push_back(*aax_id);
                }
            },
            [&](const Param_group& group) {
                for (const auto& child : group.nodes) self(child, self);
            }
        }, node);
    };
    
    visit(root, visit);
    return result;
}

} // namespace tiny