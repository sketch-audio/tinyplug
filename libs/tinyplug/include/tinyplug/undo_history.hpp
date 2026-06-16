#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "tiny_events.hpp" // User_action
#include "tiny_params.hpp" // params::Spec
#include "tiny_utils.hpp" // Inline_visitor, Processor_state

#include "action_queue.hpp"

namespace tiny {

class Undo_history {
public:

    class View {
    public:
        explicit View(Undo_history* receiver = nullptr) : _receiver{receiver} {}
        auto can_undo() const -> bool;
        auto can_redo() const -> bool;
    private:
        friend class Undo_history;
        Undo_history* _receiver{nullptr};
    };

    // An interface to trigger undo/redo actions on a receiver.
    class Actor {
    public:
        explicit Actor(Undo_history* receiver = nullptr) : _receiver{receiver} {}
        auto undo() const -> void;
        auto redo() const -> void;
        auto can_undo() const -> bool;
        auto can_redo() const -> bool;
        auto view() const -> View;
    private:
        friend class Undo_history;
        Undo_history* _receiver{nullptr};
    };

    // Process the action stream and build the undo history.
    auto process_actions(std::span<const User_action> actions, Processor_state& state) -> void;

    // Record a host-initiated bulk change (preset / full-state load) as a single
    // coalesced undo step. `before`/`after` are full param arrays in knob space;
    // any param whose value changed contributes (from=before, to=after) and is
    // appended to `out_changes` as Set_param{addr, after}. No-op (and out_changes
    // cleared) if a gesture is mid-flight or nothing changed. Clears the redo
    // stack on a real push, like an ordinary edit. Works whether or not the
    // editor is open — the caller is the long-lived wrapper class, not the view.
    // The wrapper dispatches the editor's notify() synchronously after this call,
    // folding any editor-owned marker params into the same step via amend_host_load.
    auto push_host_load(std::span<const double> before, std::span<const double> after,
                        std::vector<Set_param>& out_changes) -> void;

    // Fold an additional param change into the host-load step created by the most
    // recent push_host_load, so an editor-owned marker param (e.g. a preset-name /
    // index param) undoes together with the preset's values as a single step. If no
    // host-load step is open (none was pushed, or its diff was empty), a new
    // single-change step is created. `from`/`to` are knob space. Framework-internal:
    // the wrapper calls this from the Host_preset_loaded::add_param callback — the
    // editor never calls it directly. The open step closes when a normal gesture
    // begins (Action_start), on undo/redo, or at the next host load.
    auto amend_host_load(uint32_t addr, double from, double to) -> void;

    // Perform deferred undo/redo actions.
    auto perform_actions(Action_queue::Actor actions) -> void;

    auto can_undo() const -> bool;
    auto can_redo() const -> bool;

    auto view() -> View;

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

    // Index into _undo_stack of the host-load step that amend_host_load may still
    // extend. Reset when a normal gesture begins, on undo/redo, or at the next load.
    std::optional<size_t> _open_host_step{};

    auto undoable(uint32_t /*addr*/) const -> bool; // In the future we might want to filter some params.
    auto defer_undo() -> void;
    auto defer_redo() -> void;

    template<bool is_undo>
    auto apply(Action_queue::Actor actions) -> void;
    
};

} // namespace tiny