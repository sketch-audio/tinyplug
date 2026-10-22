#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include <tiny_plugin.hpp>

#include "plug_info.hpp"

namespace tiny::web {

// The plug-in as the page sees it: identity, I/O and the parameter model in plain space. Both modules export it.
inline auto manifest() -> std::string
{
    using Json = nlohmann::ordered_json;
    auto list = Json::array();
    for (auto addr = decltype(User_params::num_params){}; addr < User_params::num_params; ++addr) {
        const auto& spec = User_params::param_spec(addr);
        auto p = Json{
            {"address", spec.identity.address},
            {"identifier", spec.identity.identifier},
            {"name", spec.name},
            {"short_name", spec.short_name},
            {"default", params::Value_helper::default_value(spec, params::Space::Plain)},
        };
        std::visit(Inline_visitor{
            [&](const params::Semantics::Bool&) { p["kind"] = "bool"; p["min"] = 0; p["max"] = 1; },
            [&](const params::Semantics::List& s) { p["kind"] = "list"; p["min"] = 0; p["max"] = s.items.size() - 1; p["items"] = s.items; },
            [&](const params::Semantics::Int& s) { p["kind"] = "int"; p["min"] = s.min_val; p["max"] = s.max_val; },
            [&](const params::Semantics::Fixed& s) { p["kind"] = "fixed"; p["min"] = s.min_val; p["max"] = s.max_val; p["step"] = s.step_size; },
            [&](const params::Semantics::Real& s) { p["kind"] = "real"; p["min"] = s.min_val; p["max"] = s.max_val; },
        }, spec.semantics);
        list.push_back(std::move(p));
    }
    const auto doc = Json{
        {"name", Plug_info::plugin_name},
        {"identifier", Plug_info::base_identifier},
        {"version", Plug_info::version_string},
        {"variant", TINY_PLATFORM_THREADS ? "isolated" : "nonisolated"}, // What the page must provide.
        {"audio_in", Plug_info::Wants::audio_in},
        {"audio_out", Plug_info::Wants::audio_out},
        {"sidechain", Plug_info::wants_sidechain},
        {"num_meters", User_meters::num_meters},
        {"num_blocks", User_blocks::num_blocks},
        {"notes_in", Plug_info::Wants::notes_in},
        {"notes_out", Plug_info::Wants::notes_out},
        {"params", std::move(list)},
    };
    return doc.dump();
}

} // namespace tiny::web
