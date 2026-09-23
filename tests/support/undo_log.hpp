#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <variant>
#include <vector>

namespace tiny::test {

// Toy undo/redo. Two change kinds, one ordered list per step, a byte budget instead
// of a depth cap.
//
// Ordering is recorded order for both undo and redo — a step that moves a cursor and
// then edits through it only replays correctly if the cursor lands first, and the
// cursor's `from` is the value that was current when the gesture opened.
class Undo_log {
public:

    struct Param_change {
        uint32_t addr{};
        double from{};
        double to{};
    };

    struct State_change {
        std::vector<std::byte> from{};
        std::vector<std::byte> to{};
    };

    using Change = std::variant<Param_change, State_change>;

    struct Step {
        std::vector<Change> changes{};

        auto bytes() const -> size_t
        {
            auto n = changes.size() * sizeof(Change);
            for (const auto& c : changes) {
                if (const auto* s = std::get_if<State_change>(&c)) n += s->from.size() + s->to.size();
            }
            return n;
        }
    };

    explicit Undo_log(size_t budget_bytes = 4u * 1024 * 1024) : _budget{budget_bytes} {}

    // A gesture brackets everything that should undo together: a param drag, a state
    // edit, or a commit that lands while the bracket is open.
    auto begin_gesture() -> void { if (_depth++ == 0) _open = Step{}; }

    auto end_gesture() -> void
    {
        if (_depth == 0) return;
        if (--_depth != 0) return;
        if (!_open.changes.empty()) push(std::move(_open));
        _open = Step{};
    }

    auto record_param(uint32_t addr, double from, double to) -> void
    {
        if (from == to) return;
        add(Param_change{addr, from, to});
    }

    auto record_state(const std::byte* from, const std::byte* to, size_t n) -> void
    {
        auto c = State_change{};
        c.from.assign(from, from + n);
        c.to.assign(to, to + n);
        add(std::move(c));
    }

    auto can_undo() const -> bool { return !_undo.empty() && _depth == 0; }
    auto can_redo() const -> bool { return !_redo.empty() && _depth == 0; }

    // `on_param(addr, value)` and `on_state(from, to, n)`, in recorded order.
    // For undo, on_state receives (base = step.to, next = step.from): revert only what
    // this step changed, leaving anything written since alone.
    template<typename P, typename S>
    auto undo(P&& on_param, S&& on_state) -> bool
    {
        if (!can_undo()) return false;
        auto step = std::move(_undo.back());
        _undo.pop_back();
        _bytes -= step.bytes();
        replay(step, true, on_param, on_state);
        _redo.push_back(std::move(step));
        return true;
    }

    template<typename P, typename S>
    auto redo(P&& on_param, S&& on_state) -> bool
    {
        if (!can_redo()) return false;
        auto step = std::move(_redo.back());
        _redo.pop_back();
        replay(step, false, on_param, on_state);
        _bytes += step.bytes();
        _undo.push_back(std::move(step));
        return true;
    }

    auto steps() const -> size_t { return _undo.size(); }
    auto redo_steps() const -> size_t { return _redo.size(); }
    auto bytes() const -> size_t { return _bytes; }
    auto evicted() const -> size_t { return _evicted; }

private:

    template<typename C>
    auto add(C&& change) -> void
    {
        if (_depth != 0) { _open.changes.push_back(std::forward<C>(change)); return; }
        auto step = Step{};
        step.changes.push_back(std::forward<C>(change));
        push(std::move(step));
    }

    auto push(Step&& step) -> void
    {
        _bytes += step.bytes();
        _undo.push_back(std::move(step));
        _redo.clear();
        trim();
    }

    auto trim() -> void
    {
        while (_bytes > _budget && _undo.size() > 1) {
            _bytes -= _undo.front().bytes();
            _undo.pop_front();
            _evicted += 1;
        }
    }

    template<typename P, typename S>
    auto replay(const Step& step, bool is_undo, P& on_param, S& on_state) -> void
    {
        for (const auto& change : step.changes) {
            if (const auto* p = std::get_if<Param_change>(&change)) {
                on_param(p->addr, is_undo ? p->from : p->to);
            }
            else {
                const auto& s = std::get<State_change>(change);
                const auto& base = is_undo ? s.to : s.from;
                const auto& next = is_undo ? s.from : s.to;
                on_state(base.data(), next.data(), base.size());
            }
        }
    }

    size_t _budget{};
    size_t _bytes{};
    size_t _evicted{};

    int _depth{};
    Step _open{};

    std::deque<Step> _undo{};
    std::deque<Step> _redo{};

};

} // namespace tiny::test
