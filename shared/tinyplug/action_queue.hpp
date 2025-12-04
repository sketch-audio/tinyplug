#pragma once

#include <functional>
#include <span>
#include <vector>

#include "tiny_events.h" // User_action

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

} // namespace tiny