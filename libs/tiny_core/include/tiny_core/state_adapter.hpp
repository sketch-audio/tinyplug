#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "tiny_params.hpp"
#include "state_rules.hpp"
#include "edit_record.hpp"
#include "view_model.hpp" // State_item, State_map

namespace tiny {

// MARK: - Editor state
enum class State_tag : uint32_t {
    Bool = 0, Int, Double, String
};

constexpr auto tag_for(const State_item& item) -> State_tag
{
    return std::visit(Inline_visitor{
        [](const bool&) { return State_tag::Bool; },
        [](const int32_t&) { return State_tag::Int; },
        [](const double&) { return State_tag::Double; },
        [](const std::string&) { return State_tag::String; },
    }, item);
}


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

// MARK: - The framework's keys in a legacy editor map
//
// Beside the author's keys, a legacy map carries the window size (`editor_size_state`) and, in a
// factory preset the native exporters wrote, the preset's name and id. Taken out before the rest
// reaches the author as Load_info::legacy.
struct Framework_keys {
    std::optional<std::pair<int32_t, int32_t>> size{};
    std::string preset_name{};
    std::string preset_id{};
};

namespace framework_keys {

    inline constexpr auto preset_name = "tinyplug-preset-name";
    inline constexpr auto preset_id = "tinyplug-preset-id";

    inline auto take(State_map& map) -> Framework_keys
    {
        auto keys = Framework_keys{.size = editor_size_state::extract(map)};
        editor_size_state::strip(map);
        auto take_string = [&](const char* key, std::string& out) {
            if (const auto it = map.find(key); it != map.end()) {
                if (const auto* s = std::get_if<std::string>(&it->second)) out = *s;
                map.erase(it);
            }
        };
        take_string(preset_name, keys.preset_name);
        take_string(preset_id, keys.preset_id);
        return keys;
    }

    inline auto write(State_map& map, const Framework_keys& keys) -> void
    {
        if (keys.size) editor_size_state::inject(map, keys.size->first, keys.size->second);
        if (!keys.preset_name.empty()) map[preset_name] = keys.preset_name;
        if (!keys.preset_id.empty()) map[preset_id] = keys.preset_id;
    }

} // namespace framework_keys

// The framework's own keys share each container with the editor's. An editor key that claims the
// reserved prefix asserts in debug builds and is dropped in release.
inline auto drop_reserved_keys(State_map& map) -> void
{
    std::erase_if(map, [](const auto& item) {
        const auto reserved = item.first.starts_with(State_rules::reserved_prefix);
        assert(!reserved && "Editor state keys may not start with \"tinyplug-\"; the framework owns them.");
        return reserved;
    });
}

// MARK: - Adapter

// A preset's name and id: what a preset file says about itself, beside its parameters and document.
struct Preset_info {
    std::string name{};
    std::string id{};
};

// Preset and session JSON. A preset holds `version`, `params` (plain values, nested by keypath), `state`
// (the document's record, base64) and optionally `name` and `id`. A web session adds `edit` (the
// editor's record, base64) and `editor` with the framework's keys for older builds. `editor` from
// before the view model is still read: the author's keys reach the view model as Load_info::legacy.
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
        std::vector<std::byte> state_record{}; // `state::encode_record` of the document; empty for none.
        std::vector<std::byte> edit_record{};  // `encode_edit_record`; sessions only.
        State_map legacy_editor{};             // The framework's keys, for older builds; sessions only.
    };

    struct Provider {
        std::function<Load_model()> load_model{[]() { return Load_model{}; }};
        std::function<Save_model()> save_model{[]() { return Save_model{}; }};
    };

    State_adapter(const Provider& provider) : _provider{provider} {};

    // A preset: parameters, document, name and id. Never the editor's record.
    auto preset_state(const Preset_info& info = {}) const -> nlohmann::ordered_json;

    // A session (the web host's getState): a preset's keys plus the editor's record.
    auto session_state() const -> nlohmann::ordered_json;

    auto param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>;

    // The `editor` map of a file saved before the view model, framework keys included.
    auto editor_state(const nlohmann::ordered_json& preset_state) const -> State_map;

    // The document's record, for `state::decode_record`. Empty when the preset has none or it
    // is not valid base64: either way the document loads at its default.
    auto state_record(const nlohmann::ordered_json& preset_state) const -> std::vector<std::byte>;

    // A session's editor record; nullopt for a preset or anything unreadable.
    auto edit_record(const nlohmann::ordered_json& preset_state) const -> std::optional<Edit_record>;

    auto preset_info(const nlohmann::ordered_json& preset_state) const -> Preset_info;

    class Actor {
    public:
        explicit Actor(State_adapter* receiver = nullptr) : _receiver{receiver} {}
        auto preset_state(const Preset_info& info = {}) const -> nlohmann::ordered_json;
        auto param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>;
        auto editor_state(const nlohmann::ordered_json& preset_state) const -> State_map;
        auto state_record(const nlohmann::ordered_json& preset_state) const -> std::vector<std::byte>;
        auto preset_info(const nlohmann::ordered_json& preset_state) const -> Preset_info;
    private:
        State_adapter* _receiver{nullptr};
    };

    auto actor() -> Actor;

private:

    struct Keys {
        static constexpr auto version = "version";
        static constexpr auto params = "params";
        static constexpr auto editor = "editor";
        static constexpr auto state = "state";
        static constexpr auto edit = "edit";
        static constexpr auto name = "name";
        static constexpr auto id = "id";
    };

    Provider _provider{};

    static auto preset_json(const Save_model& model, const Preset_info& info) -> nlohmann::ordered_json;

};

} // namespace tiny
