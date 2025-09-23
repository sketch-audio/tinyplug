#pragma once

#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <sstream>
#include <variant>
#include <vector>

#include "AAX.h"
#include "AAX_IEffectParameters.h"
#include "AAX_CParameterManager.h"
#include "AAX_IParameter.h"

#include "tinyplug/tinyplug.h"

namespace tiny {

// MARK: - key-value helpers

using Key_tag = std::pair<std::string, int32_t>;

// Join edit keys and type tags into a single string for AAX chunk.
// Output format: "key-1:2,key-2:0"
inline auto join_keys(const State_map& map) -> std::string {
    auto oss = std::ostringstream{};
    auto first = true;
    for (const auto& [key, value] : map) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << key << ":" << enum_raw(tag_for(value));
    }
    return oss.str();
}

// Unjoin a single string into pairs of keys and type tags.
// Format: "key-1:2,key-2:0" -> {{"key-1", 2}, {"key-2", 0}}
inline auto unjoin_keys(const std::string& s) -> std::vector<Key_tag> {
    auto result = std::vector<Key_tag>{};
    auto stream = std::istringstream{s};
    auto token = std::string{};
    while (std::getline(stream, token, ',')) {
        auto pos = token.find(':');
        if (pos != std::string::npos) {
            auto key = token.substr(0, pos);
            auto tag_str = token.substr(pos + 1);
            auto tag = static_cast<int32_t>(std::stoi(tag_str));
            result.emplace_back(std::move(key), tag);
        }
    }
    return result;
}

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

// MARK: - get AAX param

template<typename ParameterManager>
inline auto get_aax_param(ParameterManager* params, uint32_t id) -> decltype(auto)
{
    if (const auto aax_id = tiny_id_to_aax(id)) {
        const auto* id_cstr = (*aax_id).c_str();
        return params->GetParameterByID(id_cstr);
    }
    return params->GetParameterByID(nullptr);
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