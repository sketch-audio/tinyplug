#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <span>
#include <type_traits>
#include <vector>

#include "state_merge.hpp"
#include "state_store.hpp"

namespace tiny::state {

// A plug-in's state is one trivially copyable struct, declared as `models::State`, that
// both sides may edit. The framework knows its size and who writes it, nothing else.
template<typename T>
concept Model = requires {
    requires std::is_trivially_copyable_v<T>;
    requires std::is_default_constructible_v<T>;
    requires alignof(T) <= 8;
    { T::writers } -> std::convertible_to<Writers>;
};

// Fallback when the plug-in has no `models/state.hpp`.
struct None {
    static constexpr auto writers = Writers::Editor;
};

template<Model M>
inline constexpr auto writers_of = static_cast<Writers>(M::writers);

// MARK: - wire

// Snapshot (processor -> editor): {Ack, T}, tagged with the processor's edit counter.
struct Ack {
    std::uint32_t applied{};  // Highest edit sequence merged in.
    std::uint32_t rejected{}; // Highest edit sequence refused.
};

// Edit (editor -> processor): {Edit_header, base, next}, tagged with a sequence. Under
// Writers::Editor it is just `next`: nothing can contest it.
struct Edit_header {
    std::uint8_t apply{};
    std::uint8_t pad[3]{};
};

template<typename T, Writers W>
inline constexpr auto edit_bytes = (W == Writers::Editor) ? sizeof(T) : sizeof(Edit_header) + 2 * sizeof(T);

template<typename T>
inline constexpr auto snapshot_bytes = sizeof(Ack) + sizeof(T);

namespace detail {

// A wire is a callable `(span, tag) -> bool` or anything with `.send(span, tag)`.
template<typename Wire>
auto send(Wire& wire, std::span<const std::byte> bytes, std::uint32_t tag) -> bool
{
    if constexpr (requires { wire.send(bytes, tag); }) return wire.send(bytes, tag);
    else return wire(bytes, tag);
}

} // namespace detail

// MARK: - processor side

template<typename T, Writers W = Writers::Both>
class Processor_side {
public:

    using Store = state::Store<T, W>;

    static constexpr auto processor_writes = (W != Writers::Editor);

    // One per host process call: applies what the editor staged, publishes on the way out.
    class Block {
    public:

        explicit Block(Processor_side* p) : _p{p}, _access{p->_store.access()} {}

        Block(const Block&) = delete;
        auto operator=(const Block&) -> Block& = delete;

        ~Block()
        {
            // Publish, THEN bump the counter readers gate on, or a reader spends a
            // generation on a buffer the change is not in yet.
            const auto dirty = _access.dirty();
            const auto rejected = _access.rejected();
            _access.finish();

            _p->_conflicts.fetch_add(_access.conflicts(), std::memory_order_relaxed);
            if (rejected) _p->_rejections.fetch_add(1, std::memory_order_relaxed);
            if (dirty) _p->_edits.fetch_add(1, std::memory_order_release);
        }

        auto get() const -> const T& { return _access.get(); }
        auto mutate() -> T& requires (processor_writes) { return _access.mutate(); }

    private:

        Processor_side* _p{};
        typename Store::Access _access;
    };

    [[nodiscard]] auto begin_block() -> Block { return Block{this}; }

    // [one requester at a time] An edit arrived. Never touches the audio thread.
    auto on_edit(std::span<const std::byte> bytes, std::uint32_t seq = 0) -> bool
    {
        if (bytes.size() != edit_bytes<T, W>) return false;

        if constexpr (!processor_writes) {
            _store.load_bytes(nullptr, bytes.data(), seq);
        }
        else {
            auto h = Edit_header{};
            std::memcpy(&h, bytes.data(), sizeof h);
            if (h.apply > static_cast<std::uint8_t>(Apply::Retry)) return false;

            const auto* base = bytes.data() + sizeof h;
            _store.load_bytes(base, base + sizeof(T), seq, static_cast<Apply>(h.apply));
        }
        return true;
    }

    // [one requester at a time, and the single reader] Replace the document. `applied` seeds the
    // ack, so an editor whose edits are already folded into `value` sees them retire. A snapshot
    // published before the load and not yet sent is skipped: it holds the document being replaced.
    auto on_session_load(const T& value, std::uint32_t applied = 0) -> void
    {
        _store.reset_to(value, applied);
        _last_sent = _edits.load(std::memory_order_acquire);
    }

    // Carries the snapshot generation across a reconstructed processor (AAX reset), so
    // the editor does not drop the next snapshots as late arrivals.
    auto seed_generation(std::uint32_t edits) -> void
    {
        _edits.store(edits, std::memory_order_release);
        _last_sent = edits;
    }

    // [single reader] Send a snapshot if anything was published since the last one.
    template<typename Wire>
    auto pump(Wire&& up) -> bool requires (processor_writes)
    {
        const auto seen = _edits.load(std::memory_order_acquire);
        if (seen == _last_sent) return false;

        _buf.resize(snapshot_bytes<T>);
        read_snapshot(_buf.data());
        if (!detail::send(up, _buf, seen)) return false;

        _last_sent = seen;
        return true;
    }

    // [single reader] {Ack, T} into `out`, for a transport that owns its own buffer.
    auto read_snapshot(std::byte* out) -> void requires (processor_writes)
    {
        auto ack = Ack{};
        _store.read_bytes(out + sizeof(Ack), ack.applied, ack.rejected);
        std::memcpy(out, &ack, sizeof(Ack));
    }

    // [single reader] The current document, answering from a staged edit if one is pending.
    auto snapshot(T& out) -> void { _store.save(out); }

    auto edits() const -> std::uint32_t { return _edits.load(std::memory_order_acquire); }
    auto conflicts() const -> std::size_t { return _conflicts.load(std::memory_order_relaxed); }
    auto rejections() const -> std::size_t { return _rejections.load(std::memory_order_relaxed); }

private:

    Store _store{};
    std::atomic<std::uint32_t> _edits{};
    std::atomic<std::size_t> _conflicts{};
    std::atomic<std::size_t> _rejections{};
    std::uint32_t _last_sent{};
    std::vector<std::byte> _buf{};

};

// What the processor holds for one block: read access always, write access only where
// the model lets the processor author. Scoped to `process`; never cache it.
template<typename T, Writers W>
class Access {
public:

    using Block = typename Processor_side<T, W>::Block;

    Access() = default;
    explicit Access(Block* block) : _block{block} {}

    auto get() const -> const T& { return _block->get(); }
    auto mutate() const -> T& requires (W != Writers::Editor) { return _block->mutate(); }

private:

    Block* _block{};

};

// MARK: - editor side

template<typename T, Writers W = Writers::Both>
class Editor_side {
public:

    static constexpr auto processor_writes = (W != Writers::Editor);
    static constexpr auto editor_writes = (W != Writers::Processor);

    // A gesture is retained and re-run whenever the view is rebuilt: on every snapshot
    // while it is outstanding, and on every retry. So capture by value, and read
    // everything from the T& rather than from a copy taken beforehand.
    using Gesture = std::function<void(T&)>;

    auto view() const -> const T& { return _view; }

    auto authoritative() const -> const T&
    {
        if constexpr (processor_writes) return _confirmed;
        else return _view;
    }

    // What the wire has accepted, and the sequence of the last edit it accepted.
    auto sent() const -> const T& { return _sent; }
    auto sent_at() const -> std::uint32_t { return _sent_at; }

    auto generation() const -> std::uint32_t { return _gen; }

    auto in_flight() const -> bool
    {
        if constexpr (processor_writes) return !_pending.empty();
        else return false;
    }

    auto pending() const -> std::size_t
    {
        if constexpr (processor_writes) return _pending.size();
        else return 0;
    }

    auto retries() const -> std::size_t { return _retries; }

    // Seed every copy, e.g. from a session. The sequence counter is not rewound.
    auto seed(const T& value) -> void
    {
        _view = _baseline = _sent = value;
        _resend = false;
        if constexpr (processor_writes) {
            _confirmed = value;
            _pending.clear();
        }
    }

    // Seed, and send the whole document on the next flush as an Overwrite: for a processor that
    // did not take the load itself.
    auto seed_and_resend(const T& value) -> void
    {
        seed(value);
        _resend = true;
    }

    // Mutate a copy of the view. Retry is the default because the dangerous edit is the
    // one that reads before it writes without looking like it: a toggle, a +=.
    template<typename F>
    auto edit(F&& f, Apply apply = Apply::Retry) -> bool requires (editor_writes)
    {
        auto next = _view;
        f(next);
        if (std::memcmp(&_view, &next, sizeof(T)) == 0) return false;

        _view = next;
        if constexpr (processor_writes) {
            _pending.push_back(Attempt{Gesture{std::forward<F>(f)}, 0, apply});
        }
        else {
            (void)apply;
        }
        return true;
    }

    // Send everything the wire has not accepted, as one patch {sent, view}.
    template<typename Wire>
    auto flush(Wire&& down) -> bool
    {
        if (!_resend && std::memcmp(&_sent, &_view, sizeof(T)) == 0) return false;

        if constexpr (!processor_writes) {
            if (!detail::send(down, {as_bytes(_view), sizeof(T)}, _sent_at + 1)) return false;
            ++_sent_at;
            _sent = _view;
            _resend = false;
            return true;
        }
        else {
            auto apply = _resend ? Apply::Overwrite : Apply::Merge;
            for (const auto& a : _pending) {
                if (a.seq == 0) apply = stricter(apply, a.apply);
            }

            _buf.resize(edit_bytes<T, W>);
            const auto h = Edit_header{static_cast<std::uint8_t>(apply), {}};
            std::memcpy(_buf.data(), &h, sizeof h);
            std::memcpy(_buf.data() + sizeof h, &_sent, sizeof(T));
            std::memcpy(_buf.data() + sizeof h + sizeof(T), &_view, sizeof(T));
            if (!detail::send(down, _buf, _sent_at + 1)) return false;

            ++_sent_at;
            _sent = _view;
            _resend = false;
            for (auto& a : _pending) {
                if (a.seq == 0) a.seq = _sent_at;
            }
            return true;
        }
    }

    // A snapshot arrived: retire what landed, re-arm what was refused, rebuild the view.
    auto on_snapshot(std::span<const std::byte> bytes, std::uint32_t gen) -> bool requires (processor_writes)
    {
        if (bytes.size() != snapshot_bytes<T>) return false;
        if (gen <= _gen) return false; // Late arrival.

        auto ack = Ack{};
        std::memcpy(&ack, bytes.data(), sizeof(Ack));
        std::memcpy(&_confirmed, bytes.data() + sizeof(Ack), sizeof(T));
        _gen = gen;

        while (!_pending.empty() && _pending.front().seq != 0 && _pending.front().seq <= ack.applied) {
            _pending.pop_front();
        }

        if (ack.rejected > _seen_rejected) {
            _seen_rejected = ack.rejected;
            if (ack.rejected > ack.applied) {
                ++_retries;
                for (auto& a : _pending) a.seq = 0;
            }
        }

        rebuild();
        return true;
    }

    // Record one undo step for everything that changed since the last commit. Diffs the
    // authoritative copy, so a step never claims an edit the processor has not accepted.
    // `history.record_state(from, to, n)`.
    template<typename History>
    auto commit(History& history) -> bool
    {
        const auto& against = authoritative();

        if (in_flight()) return false;
        if (std::memcmp(&_baseline, &against, sizeof(T)) == 0) return false;

        history.record_state(as_bytes(_baseline), as_bytes(against), sizeof(T));
        _baseline = against;
        return true;
    }

    // Undo/redo: apply the step's patch like a gesture, so it too can be refused and retried.
    auto apply_replay(const std::byte* base, const std::byte* next, std::size_t n) -> void
    {
        if (n != sizeof(T)) return;

        auto b = T{};
        auto x = T{};
        std::memcpy(&b, base, sizeof(T));
        std::memcpy(&x, next, sizeof(T));

        auto after = _view;
        merge_into(after, b, x);
        if (std::memcmp(&_view, &after, sizeof(T)) != 0) {
            _view = after;
            if constexpr (processor_writes) {
                // Retry: a blind revert over a region written since would take that write with it.
                _pending.push_back(Attempt{Gesture{[b, x](T& dst) { merge_into(dst, b, x); }}, 0, Apply::Retry});
            }
        }
        merge_into(_baseline, b, x); // So the next commit does not re-record the replay.
    }

private:

    struct Attempt {
        Gesture run{};
        std::uint32_t seq{}; // 0 = not yet handed to the wire.
        Apply apply{Apply::Retry};
    };

    // Both local copies are defined, not accumulated:
    //   sent = replay(confirmed, gestures the wire accepted)
    //   view = replay(sent,      gestures it has not)
    auto rebuild() -> void requires (processor_writes)
    {
        _sent = _confirmed;

        auto i = std::size_t{};
        for (; i < _pending.size() && _pending[i].seq != 0; ++i) _pending[i].run(_sent);

        _view = _sent;
        for (; i < _pending.size(); ++i) _pending[i].run(_view);
    }

    struct Nothing {};

    [[no_unique_address]] std::conditional_t<processor_writes, T, Nothing> _confirmed{};
    [[no_unique_address]] std::conditional_t<processor_writes, std::deque<Attempt>, Nothing> _pending{};

    T _sent{};     // What the wire has accepted.
    T _view{};     // Optimistic; what the editor draws.
    T _baseline{}; // The `from` of the next undo step.

    std::vector<std::byte> _buf{};
    std::uint32_t _gen{};
    std::uint32_t _sent_at{};
    std::uint32_t _seen_rejected{};
    std::size_t _retries{};
    bool _resend{};

};

} // namespace tiny::state
