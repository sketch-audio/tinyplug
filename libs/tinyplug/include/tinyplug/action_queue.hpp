#pragma once

#include <algorithm>
#include <functional>
#include <span>
#include <vector>

#include <tiny_core/tiny_utils.hpp> // Inline_visitor

#include "tiny_events.hpp" // User_action

namespace tiny {

class Action_queue {
public:
    // Actions and param values.
    using Observer = std::function<void(std::span<const User_action>, std::span<const double>)>;

    // An interface to push actions to a receiver.
    class Actor {
    public:
        explicit Actor(Action_queue* actions = nullptr) : _actions{actions} {}

        // Push an action to the receiver.
        auto push(const User_action& action) const -> void;

        // Sort the actions in the receiver. This can be helpful to make insertions appear as a single transaction.
        auto sort() const -> void;

        // Install an observer. Observers are called at the end of the frame with the manifested parameter values.
        auto install_observer(const Observer& observer) const -> void;

        // Clear all observers.
        auto clear_observers() const -> void;

        // Get the current contents of the action queue. Returns copy to avoid aliasing.
        auto get_actions() const -> std::vector<User_action>;

    private:
        friend class Action_queue;
        Action_queue* _actions;
    };

    // Get the current contents of the action queue.
    auto get_actions() const -> const std::vector<User_action>&;

    // Clear all actions.
    auto clear() -> void;

    // Process observers with parameter values.
    auto process_observers(std::span<const double> params) -> void;

    // Get a view to act on this action queue.
    auto actor() -> Actor;

private:

    std::vector<User_action> _actions{};
    std::vector<Observer> _observers{};

    auto push(const User_action& action) -> void;
    auto sort() -> void;
    auto install_observer(const Observer& observer) -> void;
    auto clear_observers() -> void;

};

// MARK: - implementation
inline auto Action_queue::get_actions() const -> const std::vector<User_action>&
{
    return _actions;
}

inline auto Action_queue::clear() -> void
{
    _actions.clear();
}

inline auto Action_queue::process_observers(std::span<const double> params) -> void
{
    for (const auto& observer : _observers) {
        observer(_actions, params);
    }
}

inline auto Action_queue::actor() -> Actor
{
    return Actor{this};
}

// MARK: - Actor

inline auto Action_queue::Actor::push(const User_action& action) const -> void
{
    if (_actions) {
        _actions->push(action);
    }
}

inline auto Action_queue::Actor::sort() const -> void
{
    if (_actions) {
        _actions->sort();
    }
}

inline auto Action_queue::Actor::install_observer(const Observer& observer) const -> void
{
    if (_actions) {
        _actions->install_observer(observer);
    }
}

inline auto Action_queue::Actor::clear_observers() const -> void
{
    if (_actions) {
        _actions->clear_observers();
    }
}

inline auto Action_queue::Actor::get_actions() const -> std::vector<User_action>
{
    return _actions ? _actions->get_actions() : std::vector<User_action>{};
}

// MARK: - Private

inline auto Action_queue::push(const User_action& action) -> void
{
    _actions.push_back(action);
}

inline auto Action_queue::sort() -> void
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

inline auto Action_queue::install_observer(const Observer& observer) -> void
{
    _observers.push_back(observer);
}

inline auto Action_queue::clear_observers() -> void
{
    _observers.clear();
}

} // namespace tiny
