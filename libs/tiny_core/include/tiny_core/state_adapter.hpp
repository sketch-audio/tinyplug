#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "tiny_params.hpp"
#include "state_rules.hpp"

namespace tiny {

// MARK: - Editor state
enum class State_tag : uint32_t {
    Bool = 0, Int, Double, String
};
using State_item = std::variant<bool, int32_t, double, std::string>;

constexpr auto tag_for(const State_item& item) -> State_tag
{
    return std::visit(Inline_visitor{
        [](const bool&) { return State_tag::Bool; },
        [](const int32_t&) { return State_tag::Int; },
        [](const double&) { return State_tag::Double; },
        [](const std::string&) { return State_tag::String; },
    }, item);
}

using State_map = std::unordered_map<std::string, State_item>;

// MARK: - Editor-window-size persistence (framework-owned)
//
// The editor window size is persisted through the same uniform State_map transport
// that every wrapper already serializes, under keys that ONLY the framework knows.
// The wrapper injects the size (from its own size cache) into the map at save and
// extracts it at load — the app editor never sees or emits these keys. Stored as two
// int32_t values so no Rect_size dependency leaks into this header.
namespace editor_size_state {

    inline constexpr auto width_key  = "editor-width";
    inline constexpr auto height_key = "editor-height";

    inline auto inject(State_map& map, int32_t w, int32_t h) -> void
    {
        map[width_key].emplace<int32_t>(w);
        map[height_key].emplace<int32_t>(h);
    }

    inline auto extract(const State_map& map) -> std::optional<std::pair<int32_t, int32_t>>
    {
        const auto w_it = map.find(width_key);
        const auto h_it = map.find(height_key);
        if (w_it == map.end() || h_it == map.end()) return std::nullopt;

        const auto* w = std::get_if<int32_t>(&w_it->second);
        const auto* h = std::get_if<int32_t>(&h_it->second);
        if (!w || !h) return std::nullopt;

        return std::pair{*w, *h};
    }

    inline auto strip(State_map& map) -> void
    {
        map.erase(width_key);
        map.erase(height_key);
    }

} // namespace editor_size_state

// MARK: - Adapter

class State_adapter {
public:

    struct Load_model {
        const params::Node* param_tree{nullptr};
        size_t num_params{};
    };

    struct Save_model {
        size_t version{};
        const params::Node* param_tree{nullptr};
        std::vector<double> param_values{};
        State_map editor_state{};
    };

    struct Provider {
        std::function<Load_model()> load_model{[]() { return Load_model{}; }};
        std::function<Save_model()> save_model{[]() { return Save_model{}; }};
    };

    State_adapter(const Provider& provider) : _provider{provider} {};
    auto preset_state(const State_map& extras) const -> nlohmann::ordered_json;

    auto param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>;
    auto editor_state(const nlohmann::ordered_json& preset_state) const -> State_map;

    class Actor {
    public:
        explicit Actor(State_adapter* receiver = nullptr) : _receiver{receiver} {}
        auto preset_state(const State_map& extras) const -> nlohmann::ordered_json;
        auto param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>;
        auto editor_state(const nlohmann::ordered_json& preset_state) const -> State_map;
    private:
        State_adapter* _receiver{nullptr};
    };

    auto actor() -> Actor;

private:

    struct Keys {
        static constexpr auto version = "version";
        static constexpr auto params = "params";
        static constexpr auto editor = "editor";
    };

    Provider _provider{};

};

} // namespace tiny