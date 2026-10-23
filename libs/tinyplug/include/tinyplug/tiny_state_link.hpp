#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <span>

#include <tiny_core/data_port.hpp>
#include <tiny_core/state_record.hpp>
#include <tiny_core/tiny_state.hpp>

#include "undo_history.hpp"

namespace tiny::state {

template<Model M>
using Processor_for = Processor_side<M, writers_of<M>>;

template<Model M>
using Access_for = Access<M, writers_of<M>>;

template<Model M>
class Editor_link;

// What the editor holds. Main thread only.
template<Model M>
class Editor_actor {
public:

    Editor_actor() = default;
    explicit Editor_actor(Editor_link<M>* link) : _link{link} {}

    // The document as the editor should draw it, including its own unconfirmed edits.
    auto view() const -> const M& { return _link->view(); }

    // Mutate a copy of the document. Retained and re-run if the processor refuses it, so
    // capture by value and read everything from the M&.
    template<typename F>
    auto edit(F&& f, Apply apply = Apply::Retry) const -> bool requires (writers_of<M> != Writers::Processor)
    {
        return _link->edit(std::forward<F>(f), apply);
    }

    // Close an undo step over everything edited since the last one, e.g. at gesture end.
    auto commit() const -> void { _link->commit(); }

    // True while an edit has not yet been confirmed by the processor.
    auto in_flight() const -> bool { return _link->in_flight(); }

    // Replace the document from a record, e.g. from a preset the editor loads itself, as one
    // Overwrite edit. False, and nothing changes, when the record is missing or refused.
    auto load_record(std::span<const std::byte> record) const -> bool requires (writers_of<M> != Writers::Processor)
    {
        auto doc = M{};
        if (!decode_record(record, doc)) return false;
        return _link->edit([doc](M& m) { m = doc; }, Apply::Overwrite);
    }

private:

    Editor_link<M>* _link{};

};

// The wrapper-owned editor half of the document. Lives as long as the wrapper, like its
// `Undo_history`, so edits and undo steps survive the window closing.
template<Model M>
class Editor_link {
public:

    static constexpr auto writers = writers_of<M>;
    static constexpr auto processor_writes = (writers != Writers::Editor);

    using Side = Editor_side<M, writers>;
    using Send = std::function<bool(std::span<const std::byte>, std::uint32_t)>;
    using Poll = std::function<void(Editor_link&)>;

    Editor_link() = default;
    Editor_link(const Editor_link&) = delete;
    auto operator=(const Editor_link&) -> Editor_link& = delete;

    // `down` carries edits to the processor. `poll` delivers any waiting snapshots through
    // `on_snapshot`; only needed where the processor writes.
    auto connect(Send down, Poll poll = {}) -> void
    {
        _down = std::move(down);
        _poll = std::move(poll);
    }

    auto bind(Undo_history& history) -> void
    {
        _history = &history;
        history.bind_state(Undo_history::Source::Document, Undo_history::State_hooks{
            .commit = [this]() { return _commit_now(); },
            .replay = [this](const std::byte* base, const std::byte* next, std::size_t n) {
                _side.apply_replay(base, next, n);
            }
        });
    }

    auto actor() -> Editor_actor<M> { return Editor_actor<M>{this}; }

    auto view() const -> const M& { return _side.view(); }

    template<typename F>
    auto edit(F&& f, Apply apply = Apply::Retry) -> bool requires (writers != Writers::Processor)
    {
        return _side.edit(std::forward<F>(f), apply);
    }

    // Wanted until it lands: a step waits while an edit is unconfirmed, then closes.
    auto commit() -> void
    {
        _commit_wanted = true;
        _try_commit();
    }

    auto in_flight() const -> bool { return _side.in_flight(); }

    // Once per editor frame: take in snapshots, send edits, close a waiting step. While the history
    // holds a step for this document, new edits stay local: a confirmation must credit only the held step.
    auto sync() -> void
    {
        if (_poll) _poll(*this);
        if (_down && !(_history && _history->holding(Undo_history::Source::Document))) _side.flush(_down);
        _try_commit();
        _publish();
    }

    // [any thread] The document as last published on main, at each sync and load: what a save from
    // another thread reads, since main changes the link itself without a lock.
    auto published() const -> M
    {
        const auto lock = std::lock_guard{_published_mutex};
        return _published;
    }

    // [any thread] Bumped by every publish that changed the document.
    auto published_revision() const -> std::uint32_t { return _published_revision.load(std::memory_order_acquire); }

    auto on_snapshot(std::span<const std::byte> bytes, std::uint32_t gen) -> bool requires (processor_writes)
    {
        return _side.on_snapshot(bytes, gen);
    }

    auto seed(const M& value) -> void
    {
        _side.seed(value);
        _publish();
    }

    // A host load replaced the document. Folds {view -> doc} into the load's undo step, then
    // reseeds. `Resend::Yes` for a processor that did not take the load itself: the next flush
    // carries the whole document.
    enum class Resend : bool { No, Yes };

    auto load(const M& doc, Resend resend = Resend::No) -> void
    {
        if (_poll) _poll(*this); // Take a waiting snapshot now, so it cannot land on top of the load.
        if (_history) _history->settle(Undo_history::Source::Document); // The load replaces what a held step waited for.

        const auto& was = _side.view();
        if (_history && std::memcmp(&was, &doc, sizeof(M)) != 0) {
            _history->amend_host_state(Undo_history::Source::Document, as_bytes(was), as_bytes(doc), sizeof(M));
        }

        _commit_wanted = false;
        if (resend == Resend::Yes) _side.seed_and_resend(doc);
        else _side.seed(doc);
        _publish();
    }

    // Send what is waiting now rather than at the next frame. False if the wire refused it.
    auto flush() -> bool { return _down && _side.flush(_down); }

    auto side() -> Side& { return _side; }
    auto side() const -> const Side& { return _side; }

private:

    // What `Editor_side::commit` records into: this document's source.
    struct Recorder {
        Undo_history& history;
        auto record_state(const std::byte* from, const std::byte* to, std::size_t n) -> void
        {
            history.record_state(Undo_history::Source::Document, from, to, n);
        }
    };

    // The history asks, e.g. closing a transaction. This frame's edits go out first, so a transaction
    // waits for them too; in flight, the commit waits for the confirmation.
    auto _commit_now() -> Undo_history::Commit
    {
        if (!_history) return Undo_history::Commit::None;
        if (_down && !_history->holding(Undo_history::Source::Document)) _side.flush(_down);
        if (_side.awaiting()) {
            _commit_wanted = true;
            return Undo_history::Commit::Waiting;
        }
        auto recorder = Recorder{*_history};
        return _side.commit(recorder) ? Undo_history::Commit::Recorded : Undo_history::Commit::None;
    }

    auto _try_commit() -> void
    {
        if (!_commit_wanted || !_history) return;
        // Held, only what was sent counts: later edits stay local until the held step lands.
        const auto busy = _history->holding(Undo_history::Source::Document) ? _side.awaiting() : _side.in_flight();
        if (busy) return;
        auto recorder = Recorder{*_history};
        // Nothing to record (the processor refused the edit): a step held for it lands without it.
        if (!_side.commit(recorder)) _history->settle(Undo_history::Source::Document);
        _commit_wanted = false;
    }

    auto _publish() -> void
    {
        const auto lock = std::lock_guard{_published_mutex};
        if (std::memcmp(&_published, &_side.view(), sizeof(M)) == 0) return;
        _published = _side.view();
        _published_revision.fetch_add(1, std::memory_order_release);
    }

    Side _side{};
    M _published{};
    mutable std::mutex _published_mutex{};
    std::atomic<std::uint32_t> _published_revision{};
    Undo_history* _history{};
    Send _down{};
    Poll _poll{};
    bool _commit_wanted{};

};

// Send a snapshot if the processor writes and anything changed; a no-op otherwise. A template,
// so wrappers can call it from non-template code without naming a pump that may not exist.
template<typename Processor, typename Wire>
auto pump(Processor& processor, Wire&& up) -> bool
{
    if constexpr (Processor::processor_writes) return processor.pump(std::forward<Wire>(up));
    else return false;
}

// The coupled formats (CLAP, AUv2, AUv3): both halves in one object, so the wire is a call.
template<Model M>
auto connect_in_process(Editor_link<M>& link, Processor_for<M>& processor) -> void
{
    auto down = [&processor](std::span<const std::byte> bytes, std::uint32_t seq) { return processor.on_edit(bytes, seq); };

    if constexpr (Editor_link<M>::processor_writes) {
        link.connect(down, [&processor](Editor_link<M>& l) {
            processor.pump([&l](std::span<const std::byte> bytes, std::uint32_t gen) { return l.on_snapshot(bytes, gen); });
        });
    }
    else {
        link.connect(down);
    }
}

// A snapshot as a distributed format carries it: whole, latest wins.
template<Model M>
struct Snapshot_packet {
    std::array<std::byte, snapshot_bytes<M>> bytes{};
    std::uint32_t gen{};
};

template<Model M>
using Snapshot_inbox = Data_port<Snapshot_packet<M>, Port_direction::Main>;

// The distributed formats (VST3, AAX): edits leave through `down`; snapshots arrive on any
// thread, wait in `inbox`, and `sync` takes them on the UI thread.
template<Model M>
auto connect_remote(Editor_link<M>& link, typename Editor_link<M>::Send down, Snapshot_inbox<M>& inbox) -> void
{
    if constexpr (Editor_link<M>::processor_writes) {
        link.connect(std::move(down), [&inbox](Editor_link<M>& l) {
            auto packet = Snapshot_packet<M>{};
            if (inbox.read_fresh(packet)) l.on_snapshot(packet.bytes, packet.gen);
        });
    }
    else {
        (void)inbox;
        link.connect(std::move(down));
    }
}

// [one thread at a time] Stage a received snapshot for `sync`. Drops a malformed one.
template<Model M>
auto post_snapshot(Snapshot_inbox<M>& inbox, std::span<const std::byte> bytes, std::uint32_t gen) -> bool
{
    auto packet = Snapshot_packet<M>{.gen = gen};
    if (bytes.size() != packet.bytes.size()) return false;
    std::memcpy(packet.bytes.data(), bytes.data(), bytes.size());
    inbox.write(packet);
    return true;
}

} // namespace tiny::state
