#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <variant>

#include "tiny_midi.hpp"
#include "tiny_utils.hpp"

namespace tiny::process {

// Names notes, so a processor matches on one id whatever the format supplied. A source names
// a note by an id local to it (the host's note id, the editor's own) or, with none (-1), by
// channel + key; the table mints one framework id per note at `On` and answers every later
// event for it with that id. Fixed memory. Audio thread only.
class Note_ids {
public:

    enum class Source : uint8_t { Host, Editor };

    static constexpr auto capacity = size_t{256};

    // Fill in `event`'s framework id. False to drop it: an `On` with the table full (so its `Off`
    // can't go missing), or a later event for a note the table doesn't hold.
    auto name(Source source, int32_t local, Note::Any& event) -> bool
    {
        return std::visit(Inline_visitor{
            [&](Note::On& e) { return mint(source, local, e.note); },
            [&](Note::Off& e) { return find(source, local, e.note, true); },
            [&](Note::Choke& e) { return find(source, local, e.note, true); },
            [&](Note::Expression& e) { return find(source, local, e.note, false); },
        }, event);
    }

    // Release every held note of `source` on `channel` (-1: all channels), as `Off`s.
    template<typename F>
    auto release_all(Source source, int32_t channel, F&& on_off) -> void
    {
        for (auto& held : _held) {
            if (!held.used || held.source != source) continue;
            if (channel >= 0 && held.channel != channel) continue;
            held.used = false;
            on_off(Note::Off{Note::Id{held.id, held.channel, held.key}, 0.f});
        }
    }

    // Every held note of `source` on `channel`, in no particular order.
    template<typename F>
    auto each_held(Source source, uint8_t channel, F&& on_note) const -> void
    {
        for (const auto& held : _held) {
            if (held.used && held.source == source && held.channel == channel) on_note(Note::Id{held.id, held.channel, held.key});
        }
    }

    // Forget everything, e.g. at `Reset::Hard`, where the processor releases its voices anyway.
    auto clear() -> void
    {
        for (auto& held : _held) held.used = false;
    }

private:

    struct Held {
        uint32_t id{};
        int32_t local{-1};
        uint8_t channel{};
        uint8_t key{};
        Source source{};
        bool used{};
    };

    std::array<Held, capacity> _held{};
    uint32_t _next{1};

    auto mint(Source source, int32_t local, Note::Id& note) -> bool
    {
        for (auto& held : _held) {
            if (held.used) continue;
            held = Held{_next, local, note.channel, note.key, source, true};
            note.id = _next;
            _next = (_next == UINT32_MAX) ? 1 : _next + 1;
            return true;
        }
        return false;
    }

    // A local id names its note exactly; without one, or when it names nothing held, the oldest
    // held note on channel + key answers, as hardware does.
    auto find(Source source, int32_t local, Note::Id& note, bool release) -> bool
    {
        Held* match = nullptr;
        if (local >= 0) {
            for (auto& held : _held) {
                if (held.used && held.source == source && held.local == local) { match = &held; break; }
            }
        }
        if (!match) {
            for (auto& held : _held) {
                if (!held.used || held.source != source || held.channel != note.channel || held.key != note.key) continue;
                if (!match || held.id < match->id) match = &held;
            }
        }
        if (!match) return false;

        note = Note::Id{match->id, match->channel, match->key};
        if (release) match->used = false;
        return true;
    }

};

} // namespace tiny::process
