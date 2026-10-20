#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Transports whose far end is a byte copy: AAX's private data, the web format's two modules.
namespace tiny::wire {

// MARK: - entry framing

// Every ring entry is [header][payload], each padded up to an 8-byte boundary.
struct Entry_header {
    std::uint32_t kind{};
    std::uint32_t payload_bytes{};
};
static_assert(sizeof(Entry_header) == 8, "Entry_header must stay 8 bytes.");
static_assert(std::is_trivially_copyable_v<Entry_header>);

inline constexpr auto align = std::size_t{8};

inline constexpr auto align_up(std::size_t n) -> std::size_t
{
    return (n + align - 1) & ~(align - 1);
}

// MARK: - byte ring

/*
    A single-producer / single-consumer byte ring whose far end may be reached only by
    memcpy of a byte range, with no shared pointers and no shared object identity: an
    AAX private data block through Read/WritePortDirect, or another wasm module's memory
    through the web format's pump (wrappers/web/js/wire.js mirrors this layout).

    That constraint drives the shape:

    - The layout is a fixed, standard-layout POD with published byte offsets, so the
      remote end can address `write_pos`, `read_pos` and `data` arithmetically.
    - The producer NEVER overwrites unread data; a push that would lap the consumer
      fails and the entry is dropped. This is what lets the remote consumer read at
      its leisure without a seqlock retry: everything below `write_pos` is committed
      and stays stable until the consumer itself advances `read_pos`.
    - `write_pos` / `read_pos` are free-running byte counts, never wrapped, so
      `write_pos - read_pos` is the fill level and wrapping is a masking detail.

    `Kind` names what an entry carries; it travels as a std::uint32_t.
*/
template<std::size_t Capacity, typename Kind = std::uint32_t>
struct Byte_ring {

    static_assert(std::has_single_bit(Capacity), "Byte_ring capacity must be a power of two.");
    static_assert(Capacity >= 64, "Byte_ring capacity is too small to be useful.");

    static constexpr auto capacity = Capacity;
    static constexpr auto mask = Capacity - 1;

    // Bounds the local-consumer scratch buffer, and the remote reader's parse
    // buffer. Nothing we send comes close (the largest is a worker message).
    static constexpr auto max_payload_bytes = std::size_t{512};

    // Layout is load-bearing: see offset_* below.
    std::atomic<std::uint64_t> write_pos{};
    std::atomic<std::uint64_t> read_pos{};
    unsigned char data[Capacity]{};

    // Byte offsets from the ring's start, for the remote end.
    static constexpr auto offset_write_pos = std::uint32_t{0};
    static constexpr auto offset_read_pos = std::uint32_t{8};
    static constexpr auto offset_data = std::uint32_t{16};

    // MARK: local producer

    // Returns false (and writes nothing) when the ring is full. Dropping is the
    // right failure mode for meters and blocks; callers that must not drop should
    // check the result.
    auto push(Kind kind, const void* payload, std::uint32_t payload_bytes) -> bool
    {
        const auto padded = align_up(payload_bytes);
        const auto total = sizeof(Entry_header) + padded;

        const auto w = write_pos.load(std::memory_order_relaxed);
        const auto r = read_pos.load(std::memory_order_acquire);
        if ((w - r) + total > Capacity) {
            return false;
        }

        const auto header = Entry_header{
            .kind = static_cast<std::uint32_t>(kind),
            .payload_bytes = payload_bytes
        };
        _write_wrapped(w, &header, sizeof(header));
        if (payload_bytes > 0) {
            _write_wrapped(w + sizeof(header), payload, payload_bytes);
        }

        write_pos.store(w + total, std::memory_order_release);
        return true;
    }

    template<typename T>
    auto push_value(Kind kind, const T& value) -> bool
    {
        static_assert(std::is_trivially_copyable_v<T>, "Ring payloads must be trivially copyable.");
        return push(kind, &value, static_cast<std::uint32_t>(sizeof(T)));
    }

    // MARK: local consumer

    // Calls `fn(Kind, const void* payload, std::uint32_t payload_bytes)` for each
    // pending entry, then reclaims the space.
    template<typename Fn>
    auto drain(Fn&& fn) -> void
    {
        const auto w = write_pos.load(std::memory_order_acquire);
        auto r = read_pos.load(std::memory_order_relaxed);

        alignas(8) auto scratch = std::array<unsigned char, max_payload_bytes>{};

        while (r < w) {
            auto header = Entry_header{};
            _read_wrapped(r, &header, sizeof(header));

            const auto padded = align_up(header.payload_bytes);
            const auto total = sizeof(Entry_header) + padded;

            if (header.payload_bytes > scratch.size()) {
                // Malformed / oversized: the ring is only ever written by us, so
                // this cannot happen — but resyncing is impossible, so bail out
                // and reclaim everything rather than spin.
                r = w;
                break;
            }

            if (header.payload_bytes > 0) {
                _read_wrapped(r + sizeof(header), scratch.data(), header.payload_bytes);
            }
            fn(static_cast<Kind>(header.kind), scratch.data(), header.payload_bytes);

            r += total;
        }

        read_pos.store(r, std::memory_order_release);
    }

private:

    auto _write_wrapped(std::uint64_t pos, const void* src, std::size_t bytes) -> void
    {
        const auto start = static_cast<std::size_t>(pos & mask);
        const auto first = std::min(bytes, Capacity - start);
        std::memcpy(data + start, src, first);
        if (first < bytes) {
            std::memcpy(data, static_cast<const unsigned char*>(src) + first, bytes - first);
        }
    }

    auto _read_wrapped(std::uint64_t pos, void* dst, std::size_t bytes) const -> void
    {
        const auto start = static_cast<std::size_t>(pos & mask);
        const auto first = std::min(bytes, Capacity - start);
        std::memcpy(dst, data + start, first);
        if (first < bytes) {
            std::memcpy(static_cast<unsigned char*>(dst) + first, data, bytes - first);
        }
    }

};

// The remote end addresses the ring by byte offset, so the layout assumptions
// above have to actually hold.
static_assert(std::is_standard_layout_v<Byte_ring<64>>);
static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(offsetof(Byte_ring<64>, write_pos) == Byte_ring<64>::offset_write_pos);
static_assert(offsetof(Byte_ring<64>, read_pos) == Byte_ring<64>::offset_read_pos);
static_assert(offsetof(Byte_ring<64>, data) == Byte_ring<64>::offset_data);

// MARK: - block store

/*
    One latest-value frame: a sequence word and two frame slots, read from afar by a byte copy
    with no way to write back (AAX Direct Data, the web pump), so the reader cannot take part
    in a handoff. Instead the producer only ever fills the slot the
    reader is not pointed at, and the reader brackets its copy with two reads of `seq`:

      producer   fill slots[seq & 1], then seq += 1 (release)
      reader     s = seq; copy slots[(s - 1) & 1]; accept only if seq still == s

    A publish during the copy changes `seq`, so a torn copy is always rejected and retried on
    the next wakeup. `seq == 0` means nothing has been published since the store was
    constructed — never "a frame of zeros".
*/
template<typename Frame>
struct Block_store {
    static_assert(std::is_trivially_copyable_v<Frame>);

    using Frame_type = Frame;

    static constexpr auto slot_align = std::max<std::size_t>(alignof(std::uint64_t), alignof(Frame));

    std::atomic<std::uint64_t> seq{};
    alignas(slot_align) unsigned char slots[2 * sizeof(Frame)]{};

    static constexpr auto offset_seq = std::uint32_t{0};
    static constexpr auto offset_slots = static_cast<std::uint32_t>(slot_align);
    static constexpr auto frame_bytes = static_cast<std::uint32_t>(sizeof(Frame));

    static constexpr auto offset_front(std::uint64_t s) -> std::uint32_t
    {
        return offset_slots + static_cast<std::uint32_t>((s - 1) & 1) * frame_bytes;
    }

    // The copy into the slot is relaxed atomic words: it races with the reader by design, and in wasm only atomic
    // accesses are ordered at all (Binaryen strips fences at -O2). Natively these are plain moves.
    auto publish(const Frame& frame) -> void
    {
        const auto s = seq.load(std::memory_order_relaxed);
        // The last publish's seq store must land before this copy: the slot is the one a slow reader may still be
        // copying, and it only rejects that copy if it sees seq move.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        copy_words(slot(s & 1), reinterpret_cast<const unsigned char*>(&frame), Store_words{});
        seq.store(s + 1, std::memory_order_release);
    }

    // Fill the back slot in place, for a frame too large to stage on the audio thread's stack. The fill is plain
    // writes, so this is for native readers only (AAX); a wasm reader needs `publish`.
    template<typename Fill>
    auto publish_with(Fill&& fill) -> void
    {
        const auto s = seq.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        fill(slots + (s & 1) * sizeof(Frame));
        seq.store(s + 1, std::memory_order_release);
    }

    // A local reader: the latest frame and its seq, or 0 when nothing is published or a publish overlapped the copy.
    auto read(Frame& out) const -> std::uint64_t
    {
        const auto s = seq.load(std::memory_order_acquire);
        if (s == 0) return 0;
        copy_words(reinterpret_cast<unsigned char*>(&out), slot((s - 1) & 1), Load_words{});
        std::atomic_thread_fence(std::memory_order_seq_cst); // The copy can't sink past the second load.
        return seq.load(std::memory_order_relaxed) == s ? s : 0;
    }

private:

    // Words of 4 bytes where the slot stride allows it, else bytes: atomic_ref needs its type's alignment.
    using Word = std::conditional_t<sizeof(Frame) % 4 == 0, std::uint32_t, std::uint8_t>;
    static constexpr auto num_words = sizeof(Frame) / sizeof(Word);
    struct Store_words {};
    struct Load_words {};

    auto slot(std::uint64_t i) const -> unsigned char* { return const_cast<unsigned char*>(slots) + i * sizeof(Frame); }

    static auto copy_words(unsigned char* slot, const unsigned char* src, Store_words) -> void
    {
        for (auto i = std::size_t{}; i < num_words; ++i) {
            auto w = Word{};
            std::memcpy(&w, src + i * sizeof(Word), sizeof(Word));
            std::atomic_ref<Word>{*reinterpret_cast<Word*>(slot + i * sizeof(Word))}.store(w, std::memory_order_relaxed);
        }
    }

    static auto copy_words(unsigned char* dst, unsigned char* slot, Load_words) -> void
    {
        for (auto i = std::size_t{}; i < num_words; ++i) {
            const auto w = std::atomic_ref<Word>{*reinterpret_cast<Word*>(slot + i * sizeof(Word))}.load(std::memory_order_relaxed);
            std::memcpy(dst + i * sizeof(Word), &w, sizeof(Word));
        }
    }
};

// The remote reader addresses the store by byte offset, so its layout has to be the one declared above.
template<typename Frame>
inline constexpr auto block_store_layout_ok = std::is_standard_layout_v<Block_store<Frame>>
    && offsetof(Block_store<Frame>, seq) == Block_store<Frame>::offset_seq
    && offsetof(Block_store<Frame>, slots) == Block_store<Frame>::offset_slots;

} // namespace tiny::wire
