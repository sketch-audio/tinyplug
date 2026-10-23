#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "AAX_CChunkDataParser.h"

#include <tiny_core/base64.hpp>
#include <tiny_core/state_adapter.hpp> // State_map
#include <tiny_core/state_rules.hpp>

#include "adapters.hpp" // join_keys, tiny_id_to_aax

// Our chunk's keys, written in one place for the wrapper and the preset exporter (tools/presets),
// which builds .tfx files without a plug-in instance.
namespace tiny::aax {

struct Chunk_contents {
    std::span<const float> plain_values{};    // Per address; `no_value` for one a session doesn't save.
    const State_map* editor{};                // The framework's keys, and an older preset's author keys.
    std::optional<float> bypass{};            // Absent from a preset: the host's bypass stays as it is.
    std::span<const std::byte> record{};      // The document's.
    std::span<const std::byte> edit_record{}; // The editor's; sessions only.
};

inline auto write_chunk(AAX_CChunkDataParser& parser, const Chunk_contents& c) -> void
{
    static const auto empty = State_map{};
    const auto& editor = c.editor ? *c.editor : empty;

    parser.AddInt32(State_rules::Aax::num_params, static_cast<int32_t>(c.plain_values.size()));
    parser.AddString(State_rules::Aax::edit_keys, join_keys(editor).c_str());

    for (auto i = size_t{}; i < c.plain_values.size(); ++i) {
        if (const auto id = tiny_id_to_aax(static_cast<uint32_t>(i))) parser.AddFloat(id->c_str(), c.plain_values[i]);
    }

    for (const auto& [key, value] : editor) {
        std::visit(Inline_visitor{
            [&](bool v) { parser.AddInt32(key.c_str(), v ? 1 : 0); },
            [&](int32_t v) { parser.AddInt32(key.c_str(), v); },
            [&](double v) { parser.AddDouble(key.c_str(), v); },
            [&](const std::string& v) { parser.AddString(key.c_str(), v.c_str()); },
        }, value);
    }

    if (c.bypass) parser.AddFloat(State_rules::Aax::host_bypass, *c.bypass);

    // Base64: the parser has no binary type, and its strings have no length limit.
    if (!c.record.empty()) parser.AddString(State_rules::Aax::state_record, base64::encode(c.record).c_str());
    if (!c.edit_record.empty()) parser.AddString(State_rules::Aax::edit_record, base64::encode(c.edit_record).c_str());
}

} // namespace tiny::aax
