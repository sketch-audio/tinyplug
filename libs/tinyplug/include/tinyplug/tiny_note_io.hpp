#pragma once

#include <cstdint>
#include <functional>
#include <variant>

#include <tiny_core/lock_free_queue.hpp>
#include <tiny_core/midi_codec.hpp>
#include <tiny_core/midi_mpe.hpp>
#include <tiny_core/note_ids.hpp>
#include <tiny_core/note_out.hpp>
#include <tiny_core/tiny_midi.hpp>

#include <tiny_models.hpp>

#include "tiny_processor.hpp"

namespace tiny {

#if TINY_HAS_NOTES_IN
// Plays the processor from the editor, e.g. an on-screen keyboard. A pipe: in order, delivered
// at the start of the next block through `handle`, nothing tracked. A note names itself as a
// host would: by an id of the editor's choosing (`Note::Id::id`, 0 for none, then channel +
// key), which the framework maps, so an editor note can never close a host's. False when full.
class Note_sender {
public:

    using Send = std::function<bool(const midi::Performance&)>;

    Note_sender() = default;
    explicit Note_sender(Send send) : _send{std::move(send)} {}

    auto send(const midi::Note::Any& event) const -> bool { return _send && _send(event); }
    auto send(const midi::Control::Any& event) const -> bool { return _send && _send(event); }

private:

    Send _send{};

};
#endif

} // namespace tiny

namespace tiny::process {

// The note plumbing a wrapper owns, shared by all five: identity for what comes in, the
// editor's inbox, the processor's outbox, and the stream-break rule. Audio thread, except
// `editor_inbox`'s producer side.
class Note_io {
public:

    static constexpr auto editor_capacity = size_t{256};

#if TINY_HAS_NOTES_IN
    // [editor, or the thread relaying it] Queue one event for the next block.
    auto post_from_editor(const Performance& event) -> bool { return _editor.push(event); }

    // Name a host's note event, local id -1 for none. False to drop it.
    auto from_host(int32_t local, Note::Any& event) -> bool { return _ids.name(Note_ids::Source::Host, local, event); }

    // Name a host's note event and emit it, followed, with `mpe`, by what a note starting on a
    // member channel inherits. For formats that deliver notes typed but controls as MIDI.
    template<typename F>
    auto from_host(bool mpe, int32_t local, Note::Any note, F&& emit) -> void
    {
        if (!from_host(local, note)) return;
        emit(Input{note});
        _inherit(mpe, note, emit);
    }

    // A host's wildcard off: every held host note on `channel` (-1: all), as `Off`s.
    template<typename F>
    auto release_host(int32_t channel, F&& emit) -> void
    {
        _ids.release_all(Note_ids::Source::Host, channel, [&](const Note::Off& e) { emit(Input{Note::Any{e}}); });
    }

    // One MIDI 1.0 message from the host, named, as zero or more inputs. With `mpe` (see
    // `process::mpe_enabled`), a member channel's bend, pressure and CC 74 are its notes'
    // expressions, and a note starting there inherits the channel's values.
    template<typename F>
    auto from_midi(bool mpe, uint8_t status, uint8_t d1, uint8_t d2, F&& emit) -> void
    {
#if TINY_HAS_NOTE_EXPRESSION
        _mpe.observe(status, d1, d2);
        const auto channel = static_cast<uint8_t>(status & 0x0f);
        mpe = mpe && _mpe.is_member(channel);
        if (mpe) {
            if (const auto x = _mpe.expression(status, d1)) {
                _ids.each_held(Note_ids::Source::Host, channel, [&](const Note::Id& id) {
                    emit(Input{Note::Any{Note::Expression{id, x->kind, x->value}}});
                });
                return;
            }
        }
#else
        (void)mpe;
#endif
        std::visit(Inline_visitor{
            [](std::monostate) {},
            [&](Note::Any note) {
                if (!from_host(-1, note)) return;
                emit(Input{note});
                _inherit(mpe, note, emit);
            },
            [&](const Control::Any& control) { emit(Input{control}); },
            [&](const midi::All_off& off) {
                _ids.release_all(Note_ids::Source::Host, off.channel, [&](const Note::Off& e) { emit(Input{Note::Any{e}}); });
            },
        }, midi::decode(status, d1, d2));
    }

    // Everything the editor sent since the last block, at its start.
    template<typename F>
    auto drain_editor(F&& emit) -> void
    {
        auto event = Performance{};
        while (_editor.pop(event)) {
            std::visit(Inline_visitor{
                [&](Note::Any note) {
                    const auto local = std::visit([](const auto& e) { return e.note.id; }, note);
                    const auto id = local == 0 ? int32_t{-1} : static_cast<int32_t>(local & 0x7fffffff);
                    if (_ids.name(Note_ids::Source::Editor, id, note)) emit(Input{note});
                },
                [&](const Control::Any& control) { emit(Input{control}); },
            }, event);
        }
    }
#endif

#if TINY_HAS_NOTES_OUT
    // The processor's handle for this block: refuses while the plug-in is bypassed, when the
    // wrapper passes input through instead.
    auto writer(bool bypassed) -> Note_outbox::Writer { return bypassed ? Note_outbox::Writer{} : _out.writer(); }
    auto outbox() -> Note_outbox& { return _out; }

    // Something the processor can't see broke the stream (a hard reset, bypass engaging,
    // deactivation): the wrapper sends all-notes-off at the top of its next output.
    auto request_all_off() -> void { _all_off = true; }
    auto take_all_off() -> bool { const auto was = _all_off; _all_off = false; return was; }

    // Once per block with the bypass the block renders with: an edge releases what was sounding.
    auto bypassed(bool now) -> bool
    {
        if (now != _was_bypassed) _all_off = true;
        _was_bypassed = now;
        return now;
    }
#endif

    // At `Reset::Hard`: the processor releases its voices, so the names go too.
    auto clear() -> void
    {
#if TINY_HAS_NOTES_IN
        _ids.clear();
#endif
#if TINY_HAS_NOTE_EXPRESSION
        _mpe.clear();
#endif
#if TINY_HAS_NOTES_OUT
        _all_off = true;
#endif
    }

private:

#if TINY_HAS_NOTES_IN
    template<typename F>
    auto _inherit([[maybe_unused]] bool mpe, [[maybe_unused]] const Note::Any& note, [[maybe_unused]] F& emit) -> void
    {
#if TINY_HAS_NOTE_EXPRESSION
        const auto* on = std::get_if<Note::On>(&note);
        if (!mpe || !on || !_mpe.is_member(on->note.channel)) return;
        _mpe.initial(on->note.channel, [&](Note::Expression::Kind kind, double value) {
            emit(Input{Note::Any{Note::Expression{on->note, kind, value}}});
        });
#endif
    }
#endif

#if TINY_HAS_NOTES_IN
    Note_ids _ids{};
    Lock_free_queue<Performance, editor_capacity, Queue_concurrency::spsc> _editor{};
#endif
#if TINY_HAS_NOTE_EXPRESSION
    midi::Mpe _mpe{};
#endif
#if TINY_HAS_NOTES_OUT
    Note_outbox _out{};
    bool _all_off{};
    bool _was_bypassed{};
#endif

};

} // namespace tiny::process
