#include "state_adapter.hpp"

namespace tiny {

auto State_adapter::preset_state(const State_map& extras) const -> nlohmann::ordered_json
{
    using Json = nlohmann::ordered_json;
    auto preset_state = Json{};

    const auto model = _provider.save_model();
    if (!model.param_tree) return preset_state;

    preset_state[Keys::version] = model.version;

    auto add_value = [](Json& json, const Param_spec& spec, double value) {
        const auto persistent = (spec.policy != Host_policy::interface);
        if (!persistent) return;
        const auto plain = Value_conv::knob_to_plain(value, spec.semantics);
        json[spec.string_id] = static_cast<float>(plain);
    };

    auto visit_group = [&add_value](const Param_group& group, const std::vector<double>& values, auto&& self) -> Json {
        auto group_json = Json{};
        for (const auto& node : group.nodes) {
            if (const auto* spec = std::get_if<Param_spec>(&node); spec) {
                const auto value = values[spec->address];
                add_value(group_json, *spec, value);
            }
            else if (const auto* subgroup = std::get_if<Param_group>(&node); subgroup) {
                auto subjson = self(*subgroup, values, self);
                if (!subjson.empty()) {
                    group_json[subgroup->string_id] = subjson;
                }
            }
        }
        return group_json;
    };

    if (const auto* tree = std::get_if<Param_group>(model.param_tree); tree) {
        auto params_state = visit_group(*tree, model.param_values, visit_group);
        preset_state[Keys::params] = params_state;
    }

    const auto& editor_state = model.editor_state;
    if (editor_state.size() > 0 || extras.size() > 0) {
        auto editor_json = Json{};

        // Helper
        auto do_visit = [&](const auto& key, const auto& value) {
            std::visit(Inline_visitor{
                [&](bool b) {
                    editor_json[key] = b;
                },
                [&](int32_t i) {
                    editor_json[key] = i;
                },
                [&](double d) {
                    editor_json[key] = d;
                },
                [&](const std::string& s) {
                    editor_json[key] = s;
                }
            }, value);
        };

        for (const auto& [key, value] : editor_state) {
            do_visit(key, value);
        }

        for (const auto& [key, value] : extras) {
            do_visit(key, value);
        }

        preset_state[Keys::editor] = editor_json;
    }

    return preset_state;
}

auto State_adapter::param_values(const nlohmann::ordered_json& preset_state) const -> Maybe_values<double>
{
    using Json = nlohmann::ordered_json;

    const auto model = _provider.load_model();

    auto values = Maybe_values<double>(model.num_params, std::nullopt);
    if (!model.param_tree) return values;

    auto visit_group = [](const Param_group& group, const Json& json, Maybe_values<double>& values, auto&& self) -> void {
        for (const auto& node : group.nodes) {
            if (const auto* spec = std::get_if<Param_spec>(&node); spec) {
                // Do we have a json number for this param?
                const auto has_value = json.contains(spec->string_id) && json[spec->string_id].is_number();
                if (has_value) {
                    const auto plain = json[spec->string_id].get<double>();
                    const auto knob = Value_conv::plain_to_knob(plain, spec->semantics);
                    values[spec->address] = knob;
                }
                else {
                    const auto def_val = get_knob_default(*spec);
                    values[spec->address] = def_val;
                }
            }
            else if (const auto* subgroup = std::get_if<Param_group>(&node); subgroup) {
                // Do we have a json object for this param group?
                const auto has_subjson = json.contains(subgroup->string_id) && json[subgroup->string_id].is_object();
                if (has_subjson) {
                    const auto& subjson = json[subgroup->string_id];
                    self(*subgroup, subjson, values, self);
                }
            }
        }
    };

    if (const auto* tree = std::get_if<Param_group>(model.param_tree); tree) {
        const auto params_json = preset_state.value(Keys::params, Json{}); // Value or default.
        visit_group(*tree, params_json, values, visit_group);
    }

    return values;
}

auto State_adapter::editor_state(const nlohmann::ordered_json& preset_state) const -> State_map
{
    using Json = nlohmann::ordered_json;
    const auto editor_json = preset_state.value(Keys::editor, Json{});

    auto state = State_map{};
    for (auto it = editor_json.begin(); it != editor_json.end(); ++it) {
        const auto& key = it.key();
        const auto& value = it.value();

        if (value.is_boolean()) {
            state[key] = value.get<bool>();
        }
        else if (value.is_number_integer()) {
            state[key] = static_cast<int32_t>(value.get<int64_t>());
        }
        else if (value.is_number_float()) {
            state[key] = value.get<double>();
        }
        else if (value.is_string()) {
            state[key] = value.get<std::string>();
        }
    }

    return state;
}

auto State_adapter::Actor::preset_state(const State_map& extras) const -> nlohmann::ordered_json
{
    if (_receiver) {
        return _receiver->preset_state(extras);
    } else {
        return {};
    }
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

auto State_adapter::actor() -> Actor
{
    return Actor{this};
}

} // namespace tiny