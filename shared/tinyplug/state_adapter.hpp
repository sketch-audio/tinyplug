#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tiny_params.h"
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

// MARK: - Adapter

class State_adapter {
public:

    struct Load_model {
        const Param_node* param_tree{nullptr};
        size_t num_params{};
    };

    struct Save_model {
        size_t version{};
        const Param_node* param_tree{nullptr};
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