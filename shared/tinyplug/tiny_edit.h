#pragma once

#include <functional>
#include <span>
#include <vector>

#include "tiny_events.h"
#include "tiny_params.h"
#include "tiny_queue.h"
#include "tiny_utils.h"

namespace tiny {

// Read-only access to some param (and export) values.
struct Processor_state {
    std::span<const double> params{};
    std::span<const double> meters{};
};

// MARK: action queue

struct Action_queue {

    using Observer = std::function<void(std::span<const User_action>, std::span<const double>)>;

    struct Receiver {
        explicit Receiver(Action_queue* actions = nullptr) : _actions{actions} {}
        auto push(const User_action& action) const -> void
        {
            if (_actions) {
                _actions->push(action);
            }
        }
        auto sort() const -> void
        {
            if (_actions) {
                _actions->sort();
            }
        }
        // Return copy to avoid aliasing.
        auto actions() const -> std::vector<User_action>
        {
            return _actions ? _actions->actions() : std::vector<User_action>{};
        }
        auto install_observer(const Observer& observer) const -> void
        {
            if (_actions) {
                _actions->install_observer(observer);
            }
        }
        auto clear_observers() const -> void
        {
            if (_actions) {
                _actions->clear_observers();
            }
        }
    private:
        Action_queue* _actions;
    };

    auto make_receiver() -> Receiver
    {
        return Receiver{this};
    }

    auto push(const User_action& action) -> void
    {
        _actions.push_back(action);
    }

    auto sort() -> void
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

    auto actions() const -> const std::vector<User_action>&
    {
        return _actions;
    }

    auto clear() -> void
    {
        _actions.clear();
    }

    auto install_observer(const Observer& observer) -> void
    {
        _observers.push_back(observer);
    }

    auto clear_observers() -> void
    {
        _observers.clear();
    }

    auto process_observers(std::span<const double> params) -> void
    {
        for (const auto& observer : _observers) {
            observer(_actions, params);
        }
    }

private:

    std::vector<User_action> _actions = {};

    std::vector<Observer> _observers{};

};
using Actions = Action_queue::Receiver;

// MARK: - task queue

struct Task_queue {
    using Task = std::function<void()>;
    using Queue = Lock_free_queue<Task, 16>;

    struct Receiver {
        explicit Receiver(Queue* queue = nullptr) : _queue{queue} {}
        auto push(Task task) const -> void
        {
            if (_queue) {
                [[maybe_unused]] const auto success = _queue->push(std::move(task));
                assert(success && "Queue not big enough!");
            }
        }
        auto execute_all() const -> void
        {
            if (_queue) {
                auto task = Task{};
                while (_queue->pop(task)) task();
            }
        }
    private:
        Queue* _queue{nullptr};
    };

    auto make_receiver() -> Receiver
    {
        return Receiver{&_queue};
    }

    auto execute_all() -> void
    {
        auto task = Task{};
        while (_queue.pop(task)) task();
    }

private:

    Queue _queue{};

};
using Tasks = Task_queue::Receiver;

// MARK: - undo history

struct Undo_history {

    struct Param_change {
        uint32_t addr{};
        double from{};
        double to{};
    };

    struct Undo_step {
        std::vector<Param_change> changes{};
    };

    struct Receiver {
        explicit Receiver(Undo_history* history = nullptr) : _history{history} {}
        auto undo() const -> void
        {
            if (_history) { _history->defer_undo(); }
        }
        auto redo() const -> void
        {
            if (_history) { _history->defer_redo(); }
        }
    private:
        Undo_history* _history{nullptr};
    };

    Undo_history(const std::vector<Param_spec>& specs) : _specs{specs} {}

    auto process_actions(std::span<const User_action> actions, Processor_state& state) -> void
    {
        _process_actions(actions, state);
    }

    auto undo(Actions actions) -> void
    {
        _apply<true>(actions);
    }

    auto redo(Actions actions) -> void
    {
        _apply<false>(actions);
    }

    auto make_receiver() -> Receiver
    {
        return Receiver{this};
    }

    auto defer_undo() -> void
    {
        _deferred = Deferred_action{Undo{}};
    }

    auto defer_redo() -> void
    {
        _deferred = Deferred_action{Redo{}};
    }

    auto process_deferred(Actions actions) -> void
    {
        if (_deferred) {
            std::visit(Inline_visitor{
                [&](const Undo&) {
                    _apply<true>(actions);
                },
                [&](const Redo&) {
                    _apply<false>(actions);
                }
            }, *_deferred);
            _deferred.reset();
        }
    }

private:

    std::vector<Param_spec> _specs{};

    struct Undo {}; struct Redo {};
    using Deferred_action = std::variant<Undo, Redo>;

    std::optional<Deferred_action> _deferred{};

    using Active_map = std::unordered_map<uint32_t, Param_change>;

    std::optional<Active_map> _current{};
    size_t _active{};

    std::vector<Undo_step> _undo_stack{};
    std::vector<Undo_step> _redo_stack{};

    auto undoable(uint32_t /*addr*/) const -> bool
    {
        return true;
        // const auto& spec = _specs[addr];
        // return spec.policy != Host_policy::interface;
    }

    auto _process_actions(std::span<const User_action> actions, Processor_state& state) -> void
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

    template<bool is_undo>
    auto _apply(Actions actions) -> void
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
};
using Undo_redo = Undo_history::Receiver;

struct Edit_context {
    Actions actions{};
    Tasks tasks{};
    Undo_redo undo_redo{};
};

template<typename... Args>
struct Later {

    std::function<void(Args...)> callback{[](Args...) {}};
    std::optional<Tasks> receiver{};

    template<typename... Cargs>
    void operator()(Cargs&&... args) const {
        if (receiver) {
            receiver->push([cb = callback, ...a = std::forward<Cargs>(args)]() {
                cb(std::move(a)...);
            });
        } else {
            callback(std::forward<Cargs>(args)...);
        }
    }
};

} // namespace tiny