#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace tiny::aax {

// MARK: - entry framing

// What an entry in a ring carries. The two directions use disjoint sets, but
// share one enum so the framing code stays single-source.
enum class Ring_kind : uint32_t {
    // Algorithm -> data model.
    Meter = 0,
    Worker_from_processor,
    Propose_latency,

    // Data model -> algorithm.
    Worker_to_processor,
};

// Every entry is [header][payload], each padded up to an 8-byte boundary.
struct Ring_header {
    uint32_t kind{};
    uint32_t payload_bytes{};
};
static_assert(sizeof(Ring_header) == 8, "Ring_header must stay 8 bytes.");
static_assert(std::is_trivially_copyable_v<Ring_header>);

// Payload for Ring_kind::Meter.
struct Ring_meter {
    uint32_t address{};
    uint32_t pad{};
    double value{};
};
static_assert(sizeof(Ring_meter) == 16);

// Payload for Ring_kind::Propose_latency.
struct Ring_latency {
    uint32_t samples{};
    uint32_t pad{};
};
static_assert(sizeof(Ring_latency) == 8);

inline constexpr auto ring_align = size_t{8};

inline constexpr auto ring_align_up(size_t n) -> size_t
{
    return (n + ring_align - 1) & ~(ring_align - 1);
}

// MARK: - byte ring

/*
    A single-producer / single-consumer byte ring that lives inside an AAX private
    data block, so that one of its two ends may be reached only through
    AAX_IPrivateDataAccess::Read/WritePortDirect — i.e. by memcpy of a byte range,
    with no shared pointers and no shared object identity.

    That constraint drives the shape:

    - The layout is a fixed, standard-layout POD with published byte offsets, so the
      remote end can address `write_pos`, `read_pos` and `data` arithmetically.
    - The producer NEVER overwrites unread data; a push that would lap the consumer
      fails and the entry is dropped. This is what lets the remote consumer read at
      its leisure without a seqlock retry: everything below `write_pos` is committed
      and stays stable until the consumer itself advances `read_pos`.
    - `write_pos` / `read_pos` are free-running byte counts, never wrapped, so
      `write_pos - read_pos` is the fill level and wrapping is a masking detail.

    Both directions are used. Algorithm -> data model: the algorithm is the local
    producer, the Direct Data timer is the remote consumer. Data model -> algorithm:
    the Direct Data timer is the remote producer, the algorithm is the local
    consumer.
*/
template<size_t Capacity>
struct Byte_ring {

    static_assert(std::has_single_bit(Capacity), "Byte_ring capacity must be a power of two.");
    static_assert(Capacity >= 64, "Byte_ring capacity is too small to be useful.");

    static constexpr auto capacity = Capacity;
    static constexpr auto mask = Capacity - 1;

    // Bounds the local-consumer scratch buffer, and the remote reader's parse
    // buffer. Nothing we send comes close (the largest is a worker message).
    static constexpr auto max_payload_bytes = size_t{512};

    // Layout is load-bearing: see offset_* below.
    std::atomic<uint64_t> write_pos{};
    std::atomic<uint64_t> read_pos{};
    unsigned char data[Capacity]{};

    // Byte offsets within the private data port, for the remote end.
    static constexpr auto offset_write_pos = uint32_t{0};
    static constexpr auto offset_read_pos = uint32_t{8};
    static constexpr auto offset_data = uint32_t{16};

    // MARK: local producer

    // Returns false (and writes nothing) when the ring is full. Dropping is the
    // right failure mode for meters and blocks; callers that must not drop should
    // check the result.
    auto push(Ring_kind kind, const void* payload, uint32_t payload_bytes) -> bool
    {
        const auto padded = ring_align_up(payload_bytes);
        const auto total = sizeof(Ring_header) + padded;

        const auto w = write_pos.load(std::memory_order_relaxed);
        const auto r = read_pos.load(std::memory_order_acquire);
        if ((w - r) + total > Capacity) {
            return false;
        }

        const auto header = Ring_header{
            .kind = static_cast<uint32_t>(kind),
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
    auto push_value(Ring_kind kind, const T& value) -> bool
    {
        static_assert(std::is_trivially_copyable_v<T>, "Ring payloads must be trivially copyable.");
        return push(kind, &value, static_cast<uint32_t>(sizeof(T)));
    }

    // MARK: local consumer

    // Calls `fn(Ring_kind, const void* payload, uint32_t payload_bytes)` for each
    // pending entry, then reclaims the space.
    template<typename Fn>
    auto drain(Fn&& fn) -> void
    {
        const auto w = write_pos.load(std::memory_order_acquire);
        auto r = read_pos.load(std::memory_order_relaxed);

        alignas(8) auto scratch = std::array<unsigned char, max_payload_bytes>{};

        while (r < w) {
            auto header = Ring_header{};
            _read_wrapped(r, &header, sizeof(header));

            const auto padded = ring_align_up(header.payload_bytes);
            const auto total = sizeof(Ring_header) + padded;

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
            fn(static_cast<Ring_kind>(header.kind), scratch.data(), header.payload_bytes);

            r += total;
        }

        read_pos.store(r, std::memory_order_release);
    }

private:

    auto _write_wrapped(uint64_t pos, const void* src, size_t bytes) -> void
    {
        const auto start = static_cast<size_t>(pos & mask);
        const auto first = std::min(bytes, Capacity - start);
        std::memcpy(data + start, src, first);
        if (first < bytes) {
            std::memcpy(data, static_cast<const unsigned char*>(src) + first, bytes - first);
        }
    }

    auto _read_wrapped(uint64_t pos, void* dst, size_t bytes) const -> void
    {
        const auto start = static_cast<size_t>(pos & mask);
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
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(offsetof(Byte_ring<64>, write_pos) == Byte_ring<64>::offset_write_pos);
static_assert(offsetof(Byte_ring<64>, read_pos) == Byte_ring<64>::offset_read_pos);
static_assert(offsetof(Byte_ring<64>, data) == Byte_ring<64>::offset_data);

} // namespace tiny::aax
