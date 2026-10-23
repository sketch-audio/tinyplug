#pragma once

#include <algorithm>
#include <functional>
#include <span>
#include <vector>

#include <tiny_core/tiny_utils.hpp> // Inline_visitor

#include "edit_stream.hpp"
#include "tiny_events.hpp" // User_action

namespace tiny {

class Action_queue {
public:
    // An interface to push actions to a receiver.
    class Actor {
    public:
        explicit Actor(Action_queue* actions = nullptr) : _actions{actions} {}

        // Push an action to the receiver. A set needs no gesture around it: one inside an open gesture or
        // transaction joins it, and a bare one is its own (see `Edit_stream`).
        auto push(const User_action& action) const -> void;

        // Get the current contents of the action queue. Returns copy to avoid aliasing.
        auto get_actions() const -> std::vector<User_action>;

        // The previous frame's actions as they resolved: normalized, with any undo or redo replay. What
        // to read to learn what a frame's edits did (it replaced the observer callbacks).
        auto last_frame() const -> std::vector<User_action>;

    private:
        friend class Action_queue;
        Action_queue* _actions;
    };

    // Get the current contents of the action queue.
    auto get_actions() const -> const std::vector<User_action>&;

    // Clear all actions.
    auto clear() -> void;

    // Normalize everything queued since the last call, in place.
    auto normalize() -> void;

    // End every open gesture and transaction, as when the editor's view goes away.
    auto close_all() -> void;

    // The frame resolved: keep its actions as the last frame's.
    auto finish_frame() -> void;

    // Get a view to act on this action queue.
    auto actor() -> Actor;

private:

    std::vector<User_action> _actions{};
    std::size_t _normalized{}; // _actions before this index are already normalized.
    std::vector<User_action> _scratch{};
    std::vector<User_action> _last_frame{};
    Edit_stream _stream{};

    auto push(const User_action& action) -> void;

};

// MARK: - implementation
inline auto Action_queue::get_actions() const -> const std::vector<User_action>&
{
    return _actions;
}

inline auto Action_queue::clear() -> void
{
    _actions.clear();
    _normalized = 0;
}

inline auto Action_queue::normalize() -> void
{
    _scratch.clear();
    _stream.normalize(std::span{_actions}.subspan(_normalized), _scratch);
    _actions.resize(_normalized);
    _actions.insert(_actions.end(), _scratch.begin(), _scratch.end());
    _normalized = _actions.size();
}

inline auto Action_queue::close_all() -> void
{
    normalize();
    _stream.close_all(_actions);
    _normalized = _actions.size();
}

inline auto Action_queue::finish_frame() -> void
{
    _last_frame = _actions;
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

inline auto Action_queue::Actor::get_actions() const -> std::vector<User_action>
{
    return _actions ? _actions->get_actions() : std::vector<User_action>{};
}

inline auto Action_queue::Actor::last_frame() const -> std::vector<User_action>
{
    return _actions ? _actions->_last_frame : std::vector<User_action>{};
}

// MARK: - Private

inline auto Action_queue::push(const User_action& action) -> void
{
    _actions.push_back(action);
}

} // namespace tiny
