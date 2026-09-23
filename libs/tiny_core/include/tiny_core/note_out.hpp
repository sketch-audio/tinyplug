#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "tiny_midi.hpp"

namespace tiny::process {

// What a processor sends during one host block, kept for the wrapper to hand the host after
// `process`. Fixed memory, audio thread only. `Writer` is the processor's handle, and takes
// frames relative to the current slice; the box stores them relative to the block.
class Note_outbox {
public:

    static constexpr auto capacity = size_t{1024};

    struct Entry {
        Outgoing event{};
        int32_t frame{};
        uint32_t order{};
    };

    class Writer {
    public:
        explicit Writer(Note_outbox* receiver = nullptr) : _receiver{receiver} {}

        // `frame` counts from the start of this `process` call. False when the block is full.
        auto send(int64_t frame, const Note::Any& event) const -> bool { return _receiver && _receiver->push(frame, event); }
        auto send(int64_t frame, const Control::Any& event) const -> bool { return _receiver && _receiver->push(frame, event); }
        auto send(int64_t frame, const midi::Raw& event) const -> bool { return _receiver && _receiver->push(frame, event); }

    private:
        Note_outbox* _receiver{nullptr};
    };

    auto writer() -> Writer { return Writer{this}; }

    // The wrapper, before each slice.
    auto begin_slice(int64_t start, int64_t frames) -> void
    {
        _slice_start = start;
        _slice_frames = std::max<int64_t>(frames, 1);
    }

    // Everything sent this block, in time order (send order breaks ties).
    auto events() -> std::span<const Entry>
    {
        std::sort(_entries.begin(), _entries.begin() + static_cast<std::ptrdiff_t>(_size), [](const Entry& a, const Entry& b) {
            return a.frame != b.frame ? a.frame < b.frame : a.order < b.order;
        });
        return {_entries.data(), _size};
    }

    auto clear() -> void
    {
        _size = 0;
        _slice_start = 0;
        _slice_frames = 1;
    }

    // [wrapper] At a frame of the block, not of a slice: input passed through a bypassed note effect.
    auto pass(int32_t frame, const Performance& event) -> bool
    {
        if (_size == capacity) return false;
        const auto out = std::visit([](const auto& e) { return Outgoing{e}; }, event);
        _entries[_size] = Entry{out, std::max(frame, 0), static_cast<uint32_t>(_size)};
        ++_size;
        return true;
    }

    auto push(int64_t frame, const Outgoing& event) -> bool
    {
        if (_size == capacity) return false;
        const auto clamped = std::clamp<int64_t>(frame, 0, _slice_frames - 1) + _slice_start;
        _entries[_size] = Entry{event, static_cast<int32_t>(clamped), static_cast<uint32_t>(_size)};
        ++_size;
        return true;
    }

private:

    std::array<Entry, capacity> _entries{};
    size_t _size{};
    int64_t _slice_start{};
    int64_t _slice_frames{1};

};

} // namespace tiny::process
