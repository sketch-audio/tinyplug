#pragma once

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include <tiny_core/tiny_utils.hpp> // Inline_visitor

#include "tiny_events.hpp"

namespace tiny {

// Turns the editor's actions into what wrappers and the undo history see. The editor may push a set
// with no gesture around it, a transaction (an unaddressed start/end) around sets, or a set inside
// another parameter's gesture; out of here, every set sits inside an addressed start/end on its own
// parameter, so a host gets one touch and one release per parameter, in order. Open gestures carry
// across frames. Unaddressed starts and ends pass through for the undo history; wrappers skip them.
//
//     bare set:     Set{B}                       ->  Start{B} Set{B} End{B}
//     transaction:  Start{} Set{A} Set{B} End{}  ->  Start{} Start{A} Set{A} Start{B} Set{B} End{A} End{B} End{}
//     inside {A}:   Start{A} Set{B} End{A}       ->  Start{A} Start{B} Set{B} End{B} End{A}
//
// A parameter touched implicitly (by a set inside someone else's gesture) is released when every
// open gesture has ended.
class Edit_stream {
public:

    // Append the normalized form of `in` to `out`.
    auto normalize(std::span<const User_action> in, std::vector<User_action>& out) -> void
    {
        for (const auto& action : in) {
            std::visit(Inline_visitor{
                [&](const Action_start& s) {
                    ++_depth;
                    if (s.address) {
                        grow(*s.address);
                        ++_starts[*s.address];
                        touch(*s.address, out);
                    }
                    else {
                        out.push_back(s);
                    }
                },
                [&](const Set_param& p) {
                    if (count(p.address) > 0) {
                        out.push_back(p);
                    }
                    else if (_depth > 0) {
                        touch(p.address, out);
                        _implicit.push_back(p.address);
                        out.push_back(p);
                    }
                    else {
                        out.push_back(Action_start{p.address});
                        out.push_back(p);
                        out.push_back(Action_end{p.address});
                    }
                },
                [&](const Action_end& e) {
                    if (_depth == 0) return; // A stray end: nothing is open.
                    if (e.address) {
                        if (*e.address >= _starts.size() || _starts[*e.address] == 0) return; // Never started.
                        --_starts[*e.address];
                        --_depth;
                        if (_depth == 0) release_implicit(out);
                        release(*e.address, out);
                    }
                    else {
                        --_depth;
                        if (_depth == 0) release_implicit(out);
                        out.push_back(e);
                    }
                },
                [&](const auto& other) { out.push_back(other); }
            }, action);
        }
    }

    // End everything still open, as when the editor's view goes away mid-gesture.
    auto close_all(std::vector<User_action>& out) -> void
    {
        auto addressed = uint32_t{};
        for (const auto n : _starts) addressed += n;
        release_implicit(out);
        for (auto addr = uint32_t{}; addr < _touches.size(); ++addr) {
            while (_touches[addr] > 0) release(addr, out);
            _starts[addr] = 0;
        }
        for (auto open = _depth - addressed; open > 0; --open) out.push_back(Action_end{}); // The transactions.
        _depth = 0;
    }

    auto depth() const -> uint32_t { return _depth; }

private:

    uint32_t _depth{};               // Open starts, addressed or not.
    std::vector<uint32_t> _touches{}; // Per parameter: how many open gestures touch it.
    std::vector<uint32_t> _starts{};  // Per parameter: its own open gestures, which an end must match.
    std::vector<uint32_t> _implicit{}; // Touched by a set inside another gesture.

    auto count(uint32_t addr) const -> uint32_t { return addr < _touches.size() ? _touches[addr] : 0; }

    auto grow(uint32_t addr) -> void
    {
        if (addr >= _touches.size()) {
            _touches.resize(addr + 1);
            _starts.resize(addr + 1);
        }
    }

    auto touch(uint32_t addr, std::vector<User_action>& out) -> void
    {
        grow(addr);
        if (_touches[addr]++ == 0) out.push_back(Action_start{addr});
    }

    auto release(uint32_t addr, std::vector<User_action>& out) -> void
    {
        if (count(addr) == 0) return;
        if (--_touches[addr] == 0) out.push_back(Action_end{addr});
    }

    auto release_implicit(std::vector<User_action>& out) -> void
    {
        for (const auto addr : _implicit) release(addr, out);
        _implicit.clear();
    }
};

} // namespace tiny
