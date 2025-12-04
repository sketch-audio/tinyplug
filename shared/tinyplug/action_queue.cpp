#include "action_queue.hpp"

#include <algorithm>

#include "tiny_utils.h"  // Inline_visitor

namespace tiny {

auto Action_queue::get_actions() const -> const std::vector<User_action>&
{
    return _actions;
}

auto Action_queue::clear() -> void
{
    _actions.clear();
}

auto Action_queue::process_observers(std::span<const double> params) -> void
{
    for (const auto& observer : _observers) {
        observer(_actions, params);
    }
}

auto Action_queue::actor() -> Actor
{
    return Actor{this};
}

// MARK: - Actor

auto Action_queue::Actor::push(const User_action& action) const -> void
{
    if (_actions) {
        _actions->push(action);
    }
}

auto Action_queue::Actor::sort() const -> void
{
    if (_actions) {
        _actions->sort();
    }
}

auto Action_queue::Actor::install_observer(const Observer& observer) const -> void
{
    if (_actions) {
        _actions->install_observer(observer);
    }
}

auto Action_queue::Actor::clear_observers() const -> void
{
    if (_actions) {
        _actions->clear_observers();
    }
}

auto Action_queue::Actor::get_actions() const -> std::vector<User_action>
{
    return _actions ? _actions->get_actions() : std::vector<User_action>{};
}

// MARK: - Private

auto Action_queue::push(const User_action& action) -> void
{
    _actions.push_back(action);
}

auto Action_queue::sort() -> void
{
    std::stable_sort(_actions.begin(), _actions.end(), [](const User_action& a, const User_action& b) {
        auto action_order = [](const User_action& action) -> int {
            return std::visit(Inline_visitor{
                [](const Action_start&) { return 0; },
                [](const Set_param&) { return 1; },
                [](const Action_end&) { return 2; },
                [](const auto&) { return 3; }
            }, action);
        };
        return action_order(a) < action_order(b);
    });
}

auto Action_queue::install_observer(const Observer& observer) -> void
{
    _observers.push_back(observer);
}

auto Action_queue::clear_observers() -> void
{
    _observers.clear();
}

} // namespace tiny