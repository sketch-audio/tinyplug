#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <tiny_core/tiny_params.hpp> // params::Spec
#include <tiny_core/tiny_utils.hpp> // Inline_visitor

#include "action_queue.hpp"
#include "tiny_events.hpp" // User_action

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

// MARK: - implementation
inline auto Undo_history::process_actions(std::span<const User_action> actions, Processor_state& state) -> void
{
    const auto& params = state.params;

    for (const auto& action : actions) {
        std::visit(Inline_visitor{
            [&](const Action_start& s) {
                if (!undoable(s.address)) return;
                _open_host_step.reset(); // A real gesture begins; stop amending the host-load step.
                ++_active;
                if (_active == 1) {
                    _current = Active_map{};
                }
                if (_current) {
                    _current->emplace(s.address, Param_change{
                        .addr = s.address,
                        .from = params[s.address],
                        .to = params[s.address],
                    });
                }
            },
            [&](const Set_param& p) {
                if (!undoable(p.address)) return;
                if (!_current) return;
                auto it = _current->find(p.address);
                if (it != _current->end()) {
                    it->second.to = p.value;
                }
            },
            [&](const Action_end& e) {
                if (!undoable(e.address)) return;
                if (_active == 0) return;
                --_active;
                if (_active == 0 && _current) {
                    Undo_step step{};
                    for (const auto& [_, change] : *_current) {
                        if (change.from != change.to) {
                            step.changes.push_back(change);
                        }
                    }
                    if (!step.changes.empty()) {
                        _undo_stack.push_back(std::move(step));
                        _redo_stack.clear();
                    }
                    _current.reset();
                }
            },
            [&](const auto&) {}
        }, action);
    }
}

inline auto Undo_history::push_host_load(std::span<const double> before, std::span<const double> after,
                                  std::vector<Set_param>& out_changes) -> void
{
    out_changes.clear();

    // Each load starts a fresh amend window; never extend a previous load's step.
    _open_host_step.reset();

    // A preset load mid-gesture is pathological; don't corrupt the active step.
    if (_active != 0 || _current.has_value()) return;

    const auto count = std::min(before.size(), after.size());
    constexpr auto eps = 1e-9; // Absorb host value round-tripping.

    auto step = Undo_step{};
    for (auto i = size_t{}; i < count; ++i) {
        const auto addr = static_cast<uint32_t>(i);
        if (!undoable(addr)) continue;
        if (std::abs(after[i] - before[i]) <= eps) continue;
        step.changes.push_back(Param_change{.addr = addr, .from = before[i], .to = after[i]});
        out_changes.push_back(Set_param{.address = addr, .value = after[i]});
    }

    if (!step.changes.empty()) {
        _undo_stack.push_back(std::move(step));
        _redo_stack.clear();
        _open_host_step = _undo_stack.size() - 1; // Amendable until a gesture / undo / next load.
    }
    // If the diff was empty, _open_host_step stays reset; amend_host_load will
    // create a fresh single-change step (e.g. a name-only preset load).
}

inline auto Undo_history::amend_host_load(uint32_t addr, double from, double to) -> void
{
    // Don't fold into a step while a normal gesture is mid-flight.
    if (_active != 0 || _current.has_value()) return;
    if (!undoable(addr)) return;

    auto append = [&](Undo_step& step) {
        for (auto& change : step.changes) {
            if (change.addr == addr) { change.to = to; return; } // Coalesce repeats.
        }
        step.changes.push_back(Param_change{.addr = addr, .from = from, .to = to});
    };

    if (_open_host_step && *_open_host_step < _undo_stack.size()) {
        append(_undo_stack[*_open_host_step]);
    }
    else {
        // No open host-load step (the load changed no audio params): create one.
        auto step = Undo_step{};
        step.changes.push_back(Param_change{.addr = addr, .from = from, .to = to});
        _undo_stack.push_back(std::move(step));
        _redo_stack.clear();
        _open_host_step = _undo_stack.size() - 1;
    }
}

inline auto Undo_history::perform_actions(Action_queue::Actor actions) -> void
{
    if (_deferred) {
        if (*_deferred == Deferred_action::Undo) {
            apply<true>(actions);
        }
        else {
            apply<false>(actions);
        }
        _deferred.reset();
        _open_host_step.reset(); // The stack changed; the host-load step is no longer amendable.
    }
}

inline auto Undo_history::can_undo() const -> bool
{
    return !_undo_stack.empty() && !_current.has_value(); // Outstanding changes?
}

inline auto Undo_history::can_redo() const -> bool
{
    return !_redo_stack.empty();
}

inline auto Undo_history::view() -> View
{
    return View{this};
}

inline auto Undo_history::actor() -> Actor
{
    return Actor{this};
}

// MARK: - View

inline auto Undo_history::View::can_undo() const -> bool
{
    if (_receiver) {
        return _receiver->can_undo();
    }
    return false;
}

inline auto Undo_history::View::can_redo() const -> bool
{
    if (_receiver) {
        return _receiver->can_redo();
    }
    return false;
}

// MARK: - Actor

inline auto Undo_history::Actor::undo() const -> void
{
    if (_receiver) {
        _receiver->defer_undo();
    }
}

inline auto Undo_history::Actor::redo() const -> void
{
    if (_receiver) {
        _receiver->defer_redo();
    }
}

inline auto Undo_history::Actor::can_undo() const -> bool
{
    if (_receiver) {
        return _receiver->can_undo();
    }
    return false;
}

inline auto Undo_history::Actor::can_redo() const -> bool
{
    if (_receiver) {
        return _receiver->can_redo();
    }
    return false;
}

inline auto Undo_history::Actor::view() const -> View
{
    if (_receiver) {
        return _receiver->view();
    }
    return View{nullptr};
}

// MARK: - Private

inline auto Undo_history::undoable(uint32_t /*addr*/) const -> bool
{
    return true;
}

inline auto Undo_history::defer_undo() -> void
{
    _deferred = Deferred_action::Undo;
}

inline auto Undo_history::defer_redo() -> void
{
    _deferred = Deferred_action::Redo;
}

template<bool is_undo>
inline auto Undo_history::apply(Action_queue::Actor actions) -> void
{
    auto& stack_from = is_undo ? _undo_stack : _redo_stack;
    auto& stack_to = is_undo ? _redo_stack : _undo_stack;

    if (stack_from.empty()) return;

    const auto step = stack_from.back();
    stack_from.pop_back();

    for (const auto& change : step.changes) {
        actions.push(Action_start{change.addr});
        actions.push(Set_param{change.addr, is_undo ? change.from : change.to});
        actions.push(Action_end{change.addr});
    }

    stack_to.push_back(step);
}

} // namespace tiny
