#include "undo_history.hpp"

namespace tiny {

auto Undo_history::process_actions(std::span<const User_action> actions, Processor_state& state) -> void
{
    const auto& params = state.params;

    for (const auto& action : actions) {
        std::visit(Inline_visitor{
            [&](const Action_start& s) {
                if (!undoable(s.address)) return;
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

auto Undo_history::perform_actions(Action_queue::Actor actions) -> void
{
    if (_deferred) {
        using enum Deferred_action;
        switch (*_deferred) {
            case Undo: {
                apply<true>(actions);
                break;
            }
            case Redo: {
                apply<false>(actions);
                break;
            }
        }
        _deferred.reset();
    }
}

auto Undo_history::can_undo() const -> bool
{
    return !_undo_stack.empty() && !_current.has_value(); // Outstanding changes?
}

auto Undo_history::can_redo() const -> bool
{
    return !_redo_stack.empty();
}

auto Undo_history::view() -> View
{
    return View{this};
}

auto Undo_history::actor() -> Actor
{
    return Actor{this};
}

// MARK: - View

auto Undo_history::View::can_undo() const -> bool
{
    if (_receiver) {
        return _receiver->can_undo();
    }
    return false;
}

auto Undo_history::View::can_redo() const -> bool
{
    if (_receiver) {
        return _receiver->can_redo();
    }
    return false;
}

// MARK: - Actor

auto Undo_history::Actor::undo() const -> void
{
    if (_receiver) {
        _receiver->defer_undo();
    }
}

auto Undo_history::Actor::redo() const -> void
{
    if (_receiver) {
        _receiver->defer_redo();
    }
}

auto Undo_history::Actor::can_undo() const -> bool
{
    if (_receiver) {
        return _receiver->can_undo();
    }
    return false;
}

auto Undo_history::Actor::can_redo() const -> bool
{
    if (_receiver) {
        return _receiver->can_redo();
    }
    return false;
}

auto Undo_history::Actor::view() const -> View
{
    if (_receiver) {
        return _receiver->view();
    }
    return View{nullptr};
}

// MARK: - Private

auto Undo_history::undoable(uint32_t /*addr*/) const -> bool
{
    return true;
}

auto Undo_history::defer_undo() -> void
{
    _deferred = Deferred_action::Undo;
}

auto Undo_history::defer_redo() -> void
{
    _deferred = Deferred_action::Redo;
}

template<bool is_undo>
auto Undo_history::apply(Action_queue::Actor actions) -> void
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

template auto Undo_history::apply<true>(Action_queue::Actor actions) -> void;
template auto Undo_history::apply<false>(Action_queue::Actor actions) -> void;

} // namespace tiny