#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <type_traits>

#include "state_merge.hpp"

namespace tiny::state {

// Who may author the document. Decides how much of the machinery exists at all.
//
//   Editor     only the UI authors. Patches are applied blind and nothing reports back.
//   Processor  the audio thread authors; the UI only reverts (undo/redo).
//   Both       everything.
enum class Writers : std::uint32_t { Editor, Processor, Both };

// How a staged patch is applied. A property of the gesture, not the model.
//
//   Overwrite  replace the object; anything written since is gone.
//   Merge      write the bytes the patch moved. Contested bytes are last-writer-wins.
//   Retry      refuse the patch if any byte it moves has drifted, so the editor can
//              re-run the gesture against what actually exists.
enum class Apply : std::uint8_t { Overwrite, Merge, Retry };

// Coalescing two patches keeps one policy: Overwrite absorbs everything, otherwise the stricter wins.
inline constexpr auto stricter(Apply a, Apply b) -> Apply
{
    if (a == Apply::Overwrite || b == Apply::Overwrite) return Apply::Overwrite;
    if (a == Apply::Retry || b == Apply::Retry) return Apply::Retry;
    return Apply::Merge;
}

// Processor-owned value.
//
// Outbound (audio -> reader): a triple buffer, three slots rotated through one atomic word.
// Inbound (requester -> audio): one staged {base, next} pair behind a four-state word,
// applied by the audio thread at the top of the next block.
template<typename T, Writers W = Writers::Both>
class Store {
public:

    static_assert(std::is_trivially_copyable_v<T>);

    static constexpr auto processor_writes = (W != Writers::Editor);

    // Scoped to one block. Applies whatever is staged on construction, publishes on finish.
    class Access {
    public:

        Access(const Access&) = delete;
        auto operator=(const Access&) -> Access& = delete;

        ~Access() { finish(); }

        // Publish now rather than at scope exit. Idempotent.
        auto finish() -> void
        {
            if (!_store) return;
            _store->release(_dirty, _applied);
            _store = nullptr;
        }

        auto get() const -> const T& { return _store->current().value; }

        auto mutate() -> T& requires (processor_writes)
        {
            _dirty = true;
            return _store->current().value;
        }

        auto dirty() const -> bool { return _dirty; }
        auto applied() const -> bool { return _applied; }
        auto rejected() const -> bool { return _rejected; }
        auto conflicts() const -> std::size_t { return _conflicts; }

    private:

        friend class Store;

        explicit Access(Store* store) : _store{store}
        {
            _applied = store->acquire(_conflicts, _rejected);
            _dirty = _applied || _rejected; // A refusal is reported through the next snapshot.
        }

        Store* _store{};
        std::size_t _conflicts{};
        bool _applied{};
        bool _rejected{};
        bool _dirty{};
    };

    [[nodiscard]] auto access() -> Access { return Access{this}; }

    // [single reader] The latest value, answering from a staged pair while one is in
    // flight so a load followed by a save cannot persist the old value.
    auto save(T& value) -> void
    {
        const auto guard = Reader_guard{this};

        auto expected = Stage::Staged;
        if (_stage.compare_exchange_strong(expected, Stage::Taking,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            value = _staged.desired;
            _stage.store(Stage::Staged, std::memory_order_release);
            return;
        }

        rotate();
        value = _storage[_front].value;
    }

    // [single reader] The published value and the sequence numbers it is current as of.
    // Published together, so an ack can never run ahead of the bytes it describes.
    auto read_bytes(std::byte* out, std::uint32_t& applied, std::uint32_t& rejected) -> void
    {
        const auto guard = Reader_guard{this};
        rotate();
        std::memcpy(out, &_storage[_front].value, sizeof(T));
        applied = _storage[_front].applied;
        rejected = _storage[_front].rejected;
    }

    auto read(T& value, std::uint32_t& applied, std::uint32_t& rejected) -> void
    {
        read_bytes(as_writable_bytes(value), applied, rejected);
    }

    // [one requester at a time] Stage a patch. A second load before the first is taken
    // keeps the ORIGINAL base and replaces `next`, so the earlier edit is not dropped.
    auto load_bytes(const std::byte* expected, const std::byte* desired,
                    std::uint32_t seq = 0, Apply apply = Apply::Merge) -> void
    {
        const auto was = begin_fill();

        if constexpr (!processor_writes) {
            (void)expected;
            (void)was;
            (void)apply;
            std::memcpy(&_staged.desired, desired, sizeof(T));
            _staged.seq = seq;
        }
        else if (apply == Apply::Overwrite) {
            // Replaces the object, whatever was staged: nothing of an earlier patch survives it.
            (void)expected;
            std::memcpy(&_staged.desired, desired, sizeof(T));
            _staged.expected.reset();
            _staged.seq = seq;
            _staged.apply = Apply::Overwrite;
        }
        else if (was == Stage::Staged) {
            if (_staged.is_replace()) {
                merge(as_writable_bytes(_staged.desired), expected, desired, sizeof(T));
            }
            else {
                std::memcpy(&_staged.desired, desired, sizeof(T));
            }
            _staged.seq = seq;
            _staged.apply = stricter(_staged.apply, apply);
        }
        else {
            std::memcpy(&_staged.desired, desired, sizeof(T));
            _staged.expected.emplace();
            std::memcpy(&*_staged.expected, expected, sizeof(T));
            _staged.seq = seq;
            _staged.apply = apply;
        }

        _stage.store(Stage::Staged, std::memory_order_release);
    }

    auto load(const T& expected, const T& desired, std::uint32_t seq = 0, Apply apply = Apply::Merge) -> void
    {
        load_bytes(as_bytes(expected), as_bytes(desired), seq, apply);
    }

    // [one requester at a time] Whole-object replace: a session load, nothing to preserve.
    // A nonzero `applied` becomes the published ack, for a requester whose edits `value` already holds.
    auto reset_to(const T& value, std::uint32_t applied = 0) -> void
    {
        begin_fill();
        _staged.desired = value;
        if constexpr (processor_writes) _staged.expected.reset();
        _staged.seq = applied;
        _staged.apply = Apply::Overwrite;
        _stage.store(Stage::Staged, std::memory_order_release);
    }

private:

    enum class Stage : std::uint32_t { Empty, Filling, Staged, Taking };

    struct Nothing {};

    // The base the requester diffed from. Only meaningful if someone else can write.
    using Expected = std::conditional_t<processor_writes, std::optional<T>, Nothing>;

    struct Staged {
        T desired{};
        [[no_unique_address]] Expected expected{};
        std::uint32_t seq{}; // 0 = the requester is not tracking this one.
        Apply apply{Apply::Merge};

        auto is_replace() const -> bool
        {
            if constexpr (processor_writes) return apply == Apply::Overwrite || !expected.has_value();
            else return true;
        }
    };

    struct Published {
        T value{};
        std::uint32_t applied{};  // Highest patch merged in.
        std::uint32_t rejected{}; // Highest patch refused.
    };

    // Catches two readers at once in debug builds. The reader may move between threads.
    struct Reader_guard {
        explicit Reader_guard([[maybe_unused]] Store* store)
        {
#ifndef NDEBUG
            _store = store;
            [[maybe_unused]] const auto was = _store->_reading.exchange(true, std::memory_order_acquire);
            assert(!was && "state::Store has one reader; route every read through one thread at a time.");
#endif
        }

        ~Reader_guard()
        {
#ifndef NDEBUG
            _store->_reading.store(false, std::memory_order_release);
#endif
        }

#ifndef NDEBUG
        Store* _store{};
#endif
    };

    static constexpr auto MASK_INDEX = std::uint32_t{0b011};
    static constexpr auto FLAG_DIRTY = std::uint32_t{0b100};

    auto current() -> Published& { return _storage[_back]; }

    auto rotate() -> void
    {
        if ((_ready.load(std::memory_order_acquire) & FLAG_DIRTY) != 0) {
            const auto prev = _ready.exchange(_front, std::memory_order_acq_rel);
            _front = prev & MASK_INDEX;
        }
    }

    auto begin_fill() -> Stage
    {
        for (;;) {
            auto expected = _stage.load(std::memory_order_acquire);
            if (expected == Stage::Taking) { std::this_thread::yield(); continue; }
            if (_stage.compare_exchange_weak(expected, Stage::Filling,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return expected;
            }
        }
    }

    auto acquire(std::size_t& conflicts, bool& rejected) -> bool
    {
        assert(!_open && "state::Store is already open.");
        _open = true;

        auto expected = Stage::Staged;
        if (!_stage.compare_exchange_strong(expected, Stage::Taking,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }

        if constexpr (!processor_writes) {
            (void)conflicts;
            (void)rejected;
            current().value = _staged.desired;
            note_applied();
            return true;
        }
        else {
            if (_staged.is_replace()) {
                current().value = _staged.desired;
                note_applied();
                return true;
            }

            if (_staged.apply == Apply::Retry) {
                conflicts = drift_from(current().value, *_staged.expected, _staged.desired);
                if (conflicts != 0) {
                    current().rejected = _staged.seq;
                    rejected = true;
                    return true;
                }
            }

            conflicts = merge_into(current().value, *_staged.expected, _staged.desired);
            note_applied();
            return true;
        }
    }

    // A session load carries no sequence number and must not rewind the ack.
    auto note_applied() -> void
    {
        if (_staged.seq != 0) current().applied = _staged.seq;
    }

    auto release(bool dirty, bool applied) -> void
    {
        if (dirty) publish();
        if (applied) _stage.store(Stage::Empty, std::memory_order_release);
        _open = false;
    }

    auto publish() -> void
    {
        const auto prev = _ready.exchange(FLAG_DIRTY | _back, std::memory_order_acq_rel);
        const auto index = prev & MASK_INDEX;
        _storage[index] = _storage[_back];
        _back = index;
    }

    std::array<Published, 3> _storage{};
    Staged _staged{};

    std::uint32_t _back{0};
    std::atomic<std::uint32_t> _ready{1};
    std::uint32_t _front{2};

    bool _open{false};
    std::atomic<Stage> _stage{Stage::Empty};

#ifndef NDEBUG
    std::atomic<bool> _reading{false};
#endif

};

} // namespace tiny::state
