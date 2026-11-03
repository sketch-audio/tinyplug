#include "tiny_core/state_adapter.hpp"

#include "tiny_core/base64.hpp"
#include "tiny_core/value_helper.hpp"

#include <optional>

namespace tiny {

namespace {

auto put_editor_map(nlohmann::ordered_json& json, const char* key, const State_map& map) -> void
{
    if (map.empty()) return;
    auto editor_json = nlohmann::ordered_json{};
    for (const auto& [k, value] : map) {
        std::visit([&](const auto& v) { editor_json[k] = v; }, value);
    }
    json[key] = editor_json;
}

} // namespace

auto State_adapter::preset_state(const Preset_info& info) const -> nlohmann::ordered_json
{
    return preset_json(_provider.save_model(), info);
}

auto State_adapter::preset_json(const Save_model& model, const Preset_info& info) -> nlohmann::ordered_json
{
    using namespace params;
    using Json = nlohmann::ordered_json;
    auto preset_state = Json{};

    if (!model.param_tree) return preset_state;

    preset_state[Keys::version] = model.version;
    if (!info.name.empty()) preset_state[Keys::name] = info.name;
    if (!info.id.empty()) preset_state[Keys::id] = info.id;

    auto add_value = [](Json& json, const params::Spec& spec, double value) {
        const auto persistent = (spec.policy != params::Policy::Interface);
        if (!persistent) return;
        const auto plain = params::knob_to_plain(value, spec.semantics);
        json[spec.identity.identifier] = static_cast<float>(plain);
    };

    auto visit_group = [&add_value](const params::Group& group, const std::vector<double>& values, auto&& self) -> Json {
        auto group_json = Json{};
        for (const auto& node : group.nodes) {
            if (const auto* spec = std::get_if<params::Spec>(&node); spec) {
                const auto value = values[spec->identity.address];
                add_value(group_json, *spec, value);
            }
            else if (const auto* subgroup = std::get_if<params::Group>(&node); subgroup) {
                auto subjson = self(*subgroup, values, self);
                if (!subjson.empty()) {
                    group_json[subgroup->identifier] = subjson;
                }
            }
        }
        return group_json;
    };

    if (const auto* tree = std::get_if<params::Group>(model.param_tree); tree) {
        auto params_state = visit_group(*tree, model.param_values, visit_group);
        preset_state[Keys::params] = params_state;
    }

    if (!model.state_record.empty()) {
        preset_state[Keys::state] = base64::encode(model.state_record);
    }

    return preset_state;
}

auto State_adapter::session_state() const -> nlohmann::ordered_json
{
    const auto model = _provider.save_model(); // Once: both halves from one snapshot.
    auto json = preset_json(model, {});
    put_editor_map(json, Keys::editor, model.legacy_editor);
    if (!model.edit_record.empty()) json[Keys::edit] = base64::encode(model.edit_record);
    return json;
}

auto State_adapter::param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>
{
    using namespace params;
    using Json = nlohmann::ordered_json;

    const auto model = _provider.load_model();

    auto values = Maybe_values<double>(model.num_params, std::nullopt);
    if (!model.param_tree) return values;
    // Not a preset at all (an array, a number: a hand-edited file): nothing to apply.
    if (!preset_state.is_object()) return values;

    // A preset from another version of the plug-in recalls what it can and never more than the range: a value it
    // has (by identifier) lands clamped to the param's range; one it lacks, or whose group it lacks, or that isn't a
    // number, takes the default. Identifiers it has that the plug-in doesn't are ignored.
    const auto empty = Json::object();
    // The preset's value for `spec`, in knob space and clamped to it; none when it has no number there. A JSON
    // number is always finite (nlohmann refuses 1e999 at parse), but an adapter can still make a NaN of one outside
    // its domain (a log scale given a negative): the three comparisons don't catch it, so it takes the default.
    const auto knob_of = [](const params::Spec& spec, const Json& json) -> std::optional<double> {
        const auto it = json.find(spec.identity.identifier);
        if (it == json.end() || !it->is_number()) return std::nullopt;
        const auto knob = params::plain_to_knob(it->get<double>(), spec.semantics);
        if (knob >= 0.0 && knob <= 1.0) return knob;
        if (knob > 1.0) return 1.0;
        if (knob < 0.0) return 0.0;
        return std::nullopt;
    };
    auto visit_group = [&empty, &knob_of](const params::Group& group, const Json& json, Maybe_values<double>& values, auto&& self) -> void {
        for (const auto& node : group.nodes) {
            if (const auto* spec = std::get_if<params::Spec>(&node); spec) {
                values[spec->identity.address] = knob_of(*spec, json).value_or(params::default_value(*spec, Space::Knob));
            }
            else if (const auto* subgroup = std::get_if<params::Group>(&node); subgroup) {
                const auto it = json.find(subgroup->identifier);
                self(*subgroup, it != json.end() && it->is_object() ? *it : empty, values, self);
            }
        }
    };

    if (const auto* tree = std::get_if<params::Group>(model.param_tree); tree) {
        const auto it = preset_state.find(Keys::params);
        visit_group(*tree, it != preset_state.end() && it->is_object() ? *it : empty, values, visit_group);
    }

    return values;
}

auto State_adapter::editor_state(const nlohmann::ordered_json& preset_state) const -> State_map
{
    auto state = State_map{};
    const auto found = preset_state.find(Keys::editor); // find, not value(): value() throws on a non-object
    if (found == preset_state.end() || !found->is_object()) return state;
    const auto& editor_json = *found;

    for (auto it = editor_json.begin(); it != editor_json.end(); ++it) {
        const auto& key = it.key();
        const auto& value = it.value();

        if (value.is_boolean()) {
            state[key].emplace<bool>(value.get<bool>());
        }
        else if (value.is_number_integer()) {
            state[key].emplace<int32_t>(static_cast<int32_t>(value.get<int64_t>()));
        }
        else if (value.is_number_float()) {
            state[key].emplace<double>(value.get<double>());
        }
        else if (value.is_string()) {
            state[key].emplace<std::string>(value.get<std::string>());
        }
    }

    return state;
}

auto State_adapter::state_record(const nlohmann::ordered_json& preset_state) const -> std::vector<std::byte>
{
    const auto it = preset_state.find(Keys::state);
    if (it == preset_state.end() || !it->is_string()) return {};

    auto bytes = base64::decode(it->get_ref<const std::string&>());
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

auto State_adapter::edit_record(const nlohmann::ordered_json& preset_state) const -> std::optional<Edit_record>
{
    const auto it = preset_state.find(Keys::edit);
    if (it == preset_state.end() || !it->is_string()) return std::nullopt;
    const auto bytes = base64::decode(it->get_ref<const std::string&>());
    if (!bytes) return std::nullopt;
    return decode_edit_record(*bytes);
}

auto State_adapter::preset_info(const nlohmann::ordered_json& preset_state) const -> Preset_info
{
    auto info = Preset_info{};
    if (const auto it = preset_state.find(Keys::name); it != preset_state.end() && it->is_string()) info.name = it->get<std::string>();
    if (const auto it = preset_state.find(Keys::id); it != preset_state.end() && it->is_string()) info.id = it->get<std::string>();
    return info;
}

auto State_adapter::Actor::preset_state(const Preset_info& info) const -> nlohmann::ordered_json
{
    if (_receiver) {
        return _receiver->preset_state(info);
    } else {
        return {};
    }
}

auto State_adapter::Actor::preset_info(const nlohmann::ordered_json& preset_state) const -> Preset_info
{
    return _receiver ? _receiver->preset_info(preset_state) : Preset_info{};
}

auto State_adapter::Actor::param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>
{
    if (_receiver) {
        return _receiver->param_values(preset_state);
    } else {
        return {};
    }
}

auto State_adapter::Actor::editor_state(const nlohmann::ordered_json& preset_state) const -> State_map
{
    if (_receiver) {
        return _receiver->editor_state(preset_state);
    } else {
        return {};
    }
}

auto State_adapter::Actor::state_record(const nlohmann::ordered_json& preset_state) const -> std::vector<std::byte>
{
    if (_receiver) {
        return _receiver->state_record(preset_state);
    } else {
        return {};
    }
}

auto State_adapter::actor() -> Actor
{
    return Actor{this};
}

} // namespace tiny