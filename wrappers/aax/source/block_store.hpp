#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace tiny::aax {

/*
    One block address in the algorithm's private data: a sequence word and two frame slots.

    Direct Data reads it with ReadPortDirect, a byte copy with no way to write back, so the
    reader cannot take part in a handoff. Instead the producer only ever fills the slot the
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

    auto publish(const Frame& frame) -> void
    {
        const auto s = seq.load(std::memory_order_relaxed);
        std::memcpy(slots + (s & 1) * sizeof(Frame), &frame, sizeof(Frame));
        seq.store(s + 1, std::memory_order_release);
    }
};

// ReadPortDirect addresses the store by byte offset, so its layout has to be the one declared above.
template<typename Frame>
inline constexpr auto block_store_layout_ok = std::is_standard_layout_v<Block_store<Frame>>
    && offsetof(Block_store<Frame>, seq) == Block_store<Frame>::offset_seq
    && offsetof(Block_store<Frame>, slots) == Block_store<Frame>::offset_slots;

} // namespace tiny::aax
