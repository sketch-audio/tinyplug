#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "tiny_events.h" // User_action
#include "tiny_params.h" // Param_spec
#include "tiny_utils.h" // Inline_visitor, Processor_state

#include "action_queue.hpp"

namespace tiny {

class Undo_history {
public:
    // An interface to trigger undo/redo actions on a receiver.
    class Actor {
    public:
        explicit Actor(Undo_history* receiver = nullptr) : _receiver{receiver} {}
        auto undo() const -> void;
        auto redo() const -> void;
    private:
        friend class Undo_history;
        Undo_history* _receiver{nullptr};
    };

    // Process the action stream and build the undo history.
    auto process_actions(std::span<const User_action> actions, Processor_state& state) -> void;

    // Perform deferred undo/redo actions.
    auto perform_actions(Action_queue::Actor actions) -> void;

    // Get a view to trigger undo/redo actions.
    auto actor() -> Actor;

private:

    struct Param_change {
        uint32_t addr{};
        double from{};
        double to{};
    };

    enum class Deferred_action { Undo, Redo };
    using Active_map = std::unordered_map<uint32_t, Param_change>;

    struct Undo_step {
        std::vector<Param_change> changes{};
    };

    std::optional<Deferred_action> _deferred{}; // One deferred action per frame.
    std::optional<Active_map> _current{};
    size_t _active{};

    std::vector<Undo_step> _undo_stack{};
    std::vector<Undo_step> _redo_stack{};

    auto undoable(uint32_t /*addr*/) const -> bool; // In the future we might want to filter some params.
    auto defer_undo() -> void;
    auto defer_redo() -> void;

    template<bool is_undo>
    auto apply(Action_queue::Actor actions) -> void;
    
};

} // namespace tiny