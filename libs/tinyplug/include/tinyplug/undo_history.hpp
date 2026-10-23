#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tiny_core/tiny_params.hpp> // params::Spec
#include <tiny_core/tiny_utils.hpp> // Inline_visitor

#include "action_queue.hpp"
#include "edit_types.hpp" // edit::Action::Any

namespace tiny {

class Undo_history {
public:

    // The non-parameter state an undo step can hold, at most one change of each per step.
    enum class Source : std::uint8_t { Document, View, Size };
    static constexpr auto num_sources = std::size_t{3};

    // What a source's commit did: nothing to record, recorded, or waiting for the processor to confirm.
    enum class Commit : std::uint8_t { None, Recorded, Waiting };

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

        // Everything from here to the matching end() is one undo step: across frames, around gestures,
        // and taking any document or view edit made meanwhile. Edits made before it become their own step.
        auto begin() const -> void;
        auto end() const -> void;

        template<typename F>
        auto transaction(F&& f) const -> void
        {
            begin();
            std::forward<F>(f)();
            end();
        }

    private:
        friend class Undo_history;
        Undo_history* _receiver{nullptr};
    };

    // Where begin() and end() push their actions: the queue the view's frames read.
    auto bind_actions(Action_queue::Actor actions) -> void { _actions = actions; }

    // Build the history from a frame's normalized actions (`Action_queue::normalize`).
    auto process_actions(std::span<const edit::Action::Any> actions, Processor_state& state) -> void;

    // The frame is over: a step closed during it no longer takes that frame's late edits.
    auto end_frame() -> void;

    // Record a host-initiated bulk change (preset / full-state load) as a single coalesced undo
    // step. `before`/`after` are full param arrays in knob space; any param whose value changed
    // contributes (from=before, to=after) and is appended to `out_changes` as edit::Action::Set{addr,
    // after}. No-op (and out_changes cleared) if a gesture is mid-flight. Clears the redo stack on
    // a real push, like an ordinary edit. Works whether or not the editor is open — the caller is the
    // long-lived wrapper class, not the view. The load's document and view join the step through
    // amend_host_state.
    auto push_host_load(std::span<const double> before, std::span<const double> after,
                        std::vector<edit::Action::Set>& out_changes) -> void;

    // Fold a source's change into the host-load step push_host_load opened, so one undo reverts a
    // load's params and state together. Creates the step if the load moved no params.
    // Framework-internal: `state::Editor_link::load` calls it.
    auto amend_host_state(Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void;

    // Perform deferred undo/redo actions.
    auto perform_actions(Action_queue::Actor actions) -> void;

    // How a state source takes part. `commit` records anything uncommitted, through record_state;
    // `replay` applies a step's patch {base, next}, reverting only what it changed.
    struct State_hooks {
        std::function<Commit()> commit{};
        std::function<void(const std::byte* base, const std::byte* next, std::size_t n)> replay{};
    };

    auto bind_state(Source source, State_hooks hooks) -> void { _hooks[index(source)] = std::move(hooks); }

    // A source's change, whole before and after. Joins the step being built (a gesture or transaction
    // open, or one closing), or a held step waiting for this source; otherwise it is a step of its own.
    // Clears the redo stack like any edit.
    auto record_state(Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void;

    // True while a closed step waits for `source` to confirm. The source keeps new edits local until
    // it lands, so a confirmation can't credit them to the held step.
    auto holding(Source source) const -> bool { return _held.has_value() && (_waiting & bit(source)) != 0; }

    // The source will record nothing more for the held step: its edit was refused, or a load replaced
    // it. The step lands with what it has, and the source's edits go out again.
    auto settle(Source source) -> void;

    // A gesture or transaction is open: a source's change made now belongs to the step it builds.
    auto open() const -> bool { return _active > 0 || _begun > 0; }

    // State steps cost two copies of the state, so history is capped by bytes, oldest first.
    auto set_state_budget(std::size_t bytes) -> void { _state_budget = bytes; trim(); }

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

    struct State_change {
        Source source{};
        std::vector<std::byte> from{};
        std::vector<std::byte> to{};
    };

    struct Undo_step {
        std::vector<Param_change> changes{};
        std::vector<State_change> states{};

        auto empty() const -> bool { return changes.empty() && states.empty(); }

        auto state_bytes() const -> std::size_t
        {
            auto n = std::size_t{};
            for (const auto& s : states) n += s.from.size() + s.to.size();
            return n;
        }
    };

    enum class Deferred_action { Undo, Redo };
    using Active_map = std::unordered_map<uint32_t, Param_change>;

    std::optional<Deferred_action> _deferred{};

    // The step being built while a gesture or transaction is open.
    std::optional<Active_map> _current{};
    std::vector<State_change> _current_states{};
    size_t _active{};
    bool _transaction{}; // The open step includes a transaction: closing it commits every source.
    bool _joining{};     // The open step began after a step closed this frame, and joins it if it closes this frame too.
    size_t _begun{};     // begin() calls not yet ended.
    std::vector<double> _values{};

    std::vector<Undo_step> _undo_stack{};
    std::vector<Undo_step> _redo_stack{};

    // Index into _undo_stack of the host-load step that amend_host_state may still
    // extend. Reset when a normal gesture begins, on undo/redo, or at the next load.
    std::optional<size_t> _open_host_step{};

    // Index into _undo_stack of a step closed this frame, which this frame's later edits may join.
    std::optional<size_t> _closed_this_frame{};

    // A closed step waiting for its sources to confirm, and the steps that closed behind it.
    std::optional<Undo_step> _held{};
    std::uint8_t _waiting{};
    std::vector<Undo_step> _behind{};

    Undo_step* _closing{}; // While sources commit into a step being closed.

    std::array<State_hooks, num_sources> _hooks{};
    Action_queue::Actor _actions{};
    std::size_t _state_budget{16u * 1024 * 1024};
    std::size_t _state_bytes{}; // Held by the undo stack: kept as steps come and go, so trimming costs nothing per click.

    static constexpr auto index(Source s) -> std::size_t { return static_cast<std::size_t>(s); }
    static constexpr auto bit(Source s) -> std::uint8_t { return static_cast<std::uint8_t>(1u << index(s)); }

    auto begin_transaction() -> void;
    auto end_transaction() -> void;
    auto close_step() -> void;
    auto commit_sources(Undo_step& into) -> std::uint8_t;
    auto commit_outside() -> bool;
    auto hold(Undo_step step, std::uint8_t waiting) -> void;
    auto release_held() -> void;
    auto push(Undo_step step) -> void;
    static auto add_state(std::vector<State_change>& states, Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void;
    static auto merge(Undo_step& into, Undo_step from) -> void;
    template<typename F>
    auto change_stacked(size_t index, F&& f) -> void; // A step already on the undo stack, its bytes accounted.
    auto trim() -> void;
    auto undoable(uint32_t /*addr*/) const -> bool; // In the future we might want to filter some params.
    auto defer_undo() -> void;
    auto defer_redo() -> void;

    template<bool is_undo>
    auto apply(Action_queue::Actor actions) -> void;

};

// MARK: - implementation

inline auto Undo_history::process_actions(std::span<const edit::Action::Any> actions, Processor_state& state) -> void
{
    _values.assign(state.params.begin(), state.params.end()); // Each gesture's `from` is the value as it began.

    for (const auto& action : actions) {
        std::visit(Inline_visitor{
            [&](const edit::Action::Start& s) {
                if (_active++ == 0) {
                    _current = Active_map{};
                    _current_states.clear();
                    _transaction = false;
                    _joining = _closed_this_frame.has_value();
                    _open_host_step.reset(); // A real gesture begins; stop amending the host-load step.
                }
                if (!s.address) {
                    _transaction = true;
                    return;
                }
                const auto addr = *s.address;
                if (!undoable(addr)) return;
                const auto from = addr < _values.size() ? _values[addr] : 0.;
                _current->try_emplace(addr, Param_change{.addr = addr, .from = from, .to = from});
            },
            [&](const edit::Action::Set& p) {
                if (p.address < _values.size()) _values[p.address] = p.value;
                if (!_current || !undoable(p.address)) return;
                if (auto it = _current->find(p.address); it != _current->end()) it->second.to = p.value;
            },
            [&](const edit::Action::End&) {
                if (_active == 0) return;
                if (--_active == 0) close_step();
            },
        }, action);
    }
}

inline auto Undo_history::end_frame() -> void
{
    _closed_this_frame.reset();
    _joining = false; // A gesture still open past this frame is a step of its own.
}

inline auto Undo_history::close_step() -> void
{
    auto step = Undo_step{};
    for (const auto& [_, change] : *_current) {
        if (change.from != change.to) step.changes.push_back(change);
    }
    step.states = std::move(_current_states);
    _current_states = {};
    _current.reset();

    const auto waiting = std::exchange(_transaction, false) ? commit_sources(step) : std::uint8_t{};
    const auto joining = std::exchange(_joining, false);

    if (waiting != 0) {
        hold(std::move(step), waiting);
    }
    else if (step.empty()) {
        return;
    }
    else if (_held) {
        _behind.push_back(std::move(step)); // Keeps its place after the held step.
    }
    else if (joining && _closed_this_frame && *_closed_this_frame < _undo_stack.size()) {
        change_stacked(*_closed_this_frame, [&](Undo_step& into) { merge(into, std::move(step)); });
    }
    else {
        push(std::move(step));
        _closed_this_frame = _undo_stack.size() - 1;
    }
}

inline auto Undo_history::commit_sources(Undo_step& into) -> std::uint8_t
{
    auto waiting = std::uint8_t{};
    _closing = &into;
    for (auto i = std::size_t{}; i < num_sources; ++i) {
        if (_hooks[i].commit && _hooks[i].commit() == Commit::Waiting) waiting |= static_cast<std::uint8_t>(1u << i);
    }
    _closing = nullptr;
    return waiting;
}

// Commit every source as a step of its own. False when one is still waiting to confirm.
inline auto Undo_history::commit_outside() -> bool
{
    auto pre = Undo_step{};
    const auto waiting = commit_sources(pre);
    if (waiting != 0) {
        hold(std::move(pre), waiting);
        return false;
    }
    if (!pre.empty()) push(std::move(pre));
    return true;
}

inline auto Undo_history::hold(Undo_step step, std::uint8_t waiting) -> void
{
    if (_held) merge(*_held, std::move(step));
    else _held = std::move(step);
    _waiting |= waiting;
}

inline auto Undo_history::release_held() -> void
{
    if (_held && !_held->empty()) push(std::move(*_held));
    _held.reset();
    _waiting = 0;
    for (auto& step : _behind) push(std::move(step));
    _behind.clear();
}

inline auto Undo_history::record_state(Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void
{
    if (_closing) {
        add_state(_closing->states, source, from, to, n);
        return;
    }
    if (holding(source)) {
        add_state(_held->states, source, from, to, n);
        _waiting = static_cast<std::uint8_t>(_waiting & ~bit(source));
        if (_waiting == 0) release_held();
        return;
    }
    if (_active > 0 && _current) {
        add_state(_current_states, source, from, to, n);
        return;
    }
    if (!_held && _closed_this_frame && *_closed_this_frame < _undo_stack.size()) {
        // A step closed this frame takes this frame's later edits, state as well as parameters.
        change_stacked(*_closed_this_frame, [&](Undo_step& into) { add_state(into.states, source, from, to, n); });
        return;
    }
    auto step = Undo_step{};
    add_state(step.states, source, from, to, n);
    _open_host_step.reset();
    if (_held) _behind.push_back(std::move(step));
    else push(std::move(step));
}

inline auto Undo_history::settle(Source source) -> void
{
    if (!holding(source)) return;
    _waiting = static_cast<std::uint8_t>(_waiting & ~bit(source));
    if (_waiting == 0) release_held();
}

inline auto Undo_history::push_host_load(std::span<const double> before, std::span<const double> after,
                                  std::vector<edit::Action::Set>& out_changes) -> void
{
    out_changes.clear();

    // Each load starts a fresh amend window; never extend a previous load's step.
    _open_host_step.reset();

    // A preset load mid-gesture is pathological; don't corrupt the active step.
    if (_active != 0 || _current.has_value()) return;

    // A step still held was made before the load: it lands first, below the load's.
    if (_held) release_held();

    const auto count = std::min(before.size(), after.size());
    constexpr auto eps = 1e-9; // Absorb host value round-tripping.

    auto step = Undo_step{};
    for (auto i = size_t{}; i < count; ++i) {
        const auto addr = static_cast<uint32_t>(i);
        if (!undoable(addr)) continue;
        if (std::abs(after[i] - before[i]) <= eps) continue;
        step.changes.push_back(Param_change{.addr = addr, .from = before[i], .to = after[i]});
        out_changes.push_back(edit::Action::Set{.value = after[i], .address = addr});
    }

    if (!step.changes.empty()) {
        push(std::move(step));
        _open_host_step = _undo_stack.size() - 1; // Amendable until a gesture / undo / next load.
    }
    // If the diff was empty, _open_host_step stays reset; amend_host_state creates a
    // fresh step for a load that moved only state (e.g. a view-only change).
}

inline auto Undo_history::amend_host_state(Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void
{
    if (_active != 0 || _current.has_value()) return;

    if (_open_host_step && *_open_host_step < _undo_stack.size()) {
        change_stacked(*_open_host_step, [&](Undo_step& into) { add_state(into.states, source, from, to, n); });
    }
    else {
        if (_held) release_held(); // Made before the load: below it.
        auto step = Undo_step{};
        add_state(step.states, source, from, to, n);
        push(std::move(step));
        _open_host_step = _undo_stack.size() - 1;
    }
}

inline auto Undo_history::push(Undo_step step) -> void
{
    _state_bytes += step.state_bytes();
    _undo_stack.push_back(std::move(step));
    _redo_stack.clear();
    trim();
}

template<typename F>
inline auto Undo_history::change_stacked(size_t index, F&& f) -> void
{
    auto& step = _undo_stack[index];
    _state_bytes -= step.state_bytes();
    f(step);
    _state_bytes += step.state_bytes();
    trim();
}

// Coalesce: a source already in the list keeps its first `from` and takes the new `to`.
inline auto Undo_history::add_state(std::vector<State_change>& states, Source source, const std::byte* from, const std::byte* to, std::size_t n) -> void
{
    for (auto& s : states) {
        if (s.source == source) {
            s.to.assign(to, to + n);
            return;
        }
    }
    states.push_back(State_change{source, {from, from + n}, {to, to + n}});
}

inline auto Undo_history::merge(Undo_step& into, Undo_step from) -> void
{
    for (const auto& change : from.changes) {
        auto it = std::ranges::find(into.changes, change.addr, &Param_change::addr);
        if (it != into.changes.end()) it->to = change.to;
        else into.changes.push_back(change);
    }
    for (const auto& s : from.states) add_state(into.states, s.source, s.from.data(), s.to.data(), s.to.size());
}

inline auto Undo_history::trim() -> void
{
    if (_state_bytes <= _state_budget) return;

    auto evict = std::size_t{};
    while (_state_bytes > _state_budget && evict + 1 < _undo_stack.size()) {
        _state_bytes -= _undo_stack[evict].state_bytes();
        ++evict;
    }
    if (evict == 0) return;

    _undo_stack.erase(_undo_stack.begin(), _undo_stack.begin() + static_cast<std::ptrdiff_t>(evict));
    for (auto* index : {&_open_host_step, &_closed_this_frame}) {
        if (!*index) continue;
        if (**index < evict) index->reset();
        else **index -= evict;
    }
}

inline auto Undo_history::perform_actions(Action_queue::Actor actions) -> void
{
    if (!_deferred) return;
    const auto deferred = *std::exchange(_deferred, std::nullopt);
    if (_active != 0 || _held) return; // Mid-gesture, or a step not yet landed: nothing to undo into.
    if (!commit_outside()) return;     // Anything uncommitted becomes a step first.

    if (deferred == Deferred_action::Undo) apply<true>(actions);
    else apply<false>(actions);

    _open_host_step.reset(); // The stack changed; the host-load step is no longer amendable.
    _closed_this_frame.reset();
}

inline auto Undo_history::begin_transaction() -> void
{
    if (_begun++ == 0 && _active == 0) commit_outside();
    _actions.push(edit::Action::Start{});
}

inline auto Undo_history::end_transaction() -> void
{
    if (_begun == 0) return;
    --_begun;
    _actions.push(edit::Action::End{});
}

inline auto Undo_history::can_undo() const -> bool
{
    return !_undo_stack.empty() && !_current.has_value() && !_held.has_value(); // Outstanding changes?
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

inline auto Undo_history::Actor::begin() const -> void
{
    if (_receiver) {
        _receiver->begin_transaction();
    }
}

inline auto Undo_history::Actor::end() const -> void
{
    if (_receiver) {
        _receiver->end_transaction();
    }
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

    auto step = std::move(stack_from.back());
    stack_from.pop_back();

    for (const auto& change : step.changes) {
        actions.push(edit::Action::Start{change.addr});
        actions.push(edit::Action::Set{.value = is_undo ? change.from : change.to, .address = change.addr});
        actions.push(edit::Action::End{change.addr});
    }

    for (const auto& state : step.states) {
        // Undo reverts {to -> from}; redo re-applies {from -> to}.
        const auto& replay = _hooks[index(state.source)].replay;
        if (!replay) continue;
        const auto& base = is_undo ? state.to : state.from;
        const auto& next = is_undo ? state.from : state.to;
        replay(base.data(), next.data(), base.size());
    }

    if constexpr (is_undo) _state_bytes -= step.state_bytes(); // Only the undo stack counts.
    else _state_bytes += step.state_bytes();
    stack_to.push_back(std::move(step));
    if constexpr (!is_undo) trim();
}

} // namespace tiny
