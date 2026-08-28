#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "tiny_meters.hpp"

namespace tiny::meters {

// What one address holds for the editor's next read.
struct Sample {
    float value{};        // Level, peak-since-last-read, or the last trigger's magnitude.
    uint32_t triggers{};  // Trig only: how many fired since the previous read.
};

// The editor-facing transport: one slot per meter address, combined per policy.
//
// This replaces a queue, and the reason is that a queue is the wrong shape for two
// of the three policies. A `Stream` is a level and a `Peak` is a maximum over an
// interval; both *coalesce*, so a transport that preserves every intermediate value
// is spending capacity to carry data the editor will never draw — and when that
// capacity runs out (an editor closed, or merely stalled behind an occluded window,
// since the queue only drains during the draw loop) it drops the values that matter
// while holding the ones that don't.
//
// A slot array has no capacity to run out. Each policy's combine is the coalescing
// the editor would have had to do anyway:
//
//   Stream  store  — last value wins; retained, so a reader that has been away
//                    still finds the current level waiting.
//   Peak    max    — and the reader *takes* it, clearing the slot. "Maximum since
//                    you last looked" is what a peak meter means, and it stays true
//                    whether "since" is one frame or ten minutes. A post counter
//                    separates "nothing arrived" from "silence": without it, a
//                    reader running faster than the transport delivers reads its
//                    own empty slot as zero and the meter flickers. VST3 makes that
//                    routine — the host forwards output parameters to the controller
//                    at whatever rate it likes, which need not be the frame rate.
//   Trig    count  — a monotonic counter the reader diffs, so events neither
//                    coalesce nor need a queue of their own. The magnitude rides
//                    alongside and is advisory: the count is the truth.
//
// Single producer (the audio thread), single consumer (whichever thread draws).
template<typename User_meters>
class Mailbox {
public:

    static constexpr auto num_meters = User_meters::num_meters;

    // Combine one block's contribution. Never blocks and cannot fail — there is no
    // capacity to overflow, which is the entire point of the shape.
    //
    // [audio thread]
    auto post(uint32_t address, float value) -> void
    {
        auto& slot = _slots[address];

        switch (User_meters::spec(address).policy) {
            case Policy::Stream: {
                // A level carries no "arrived" signal: the reader never clears it, so
                // the last store is always the current answer.
                slot.store(Slot{value, 0}, std::memory_order_release);
                break;
            }
            case Policy::Peak: {
                // The comparison is explicit rather than std::max so a NaN from a
                // misbehaving effect is dropped instead of poisoning the slot.
                auto cur = slot.load(std::memory_order_relaxed);
                auto next = Slot{};
                do {
                    next = Slot{value > cur.value ? value : cur.value, cur.count + 1};
                } while (!slot.compare_exchange_weak(cur, next,
                             std::memory_order_release, std::memory_order_relaxed));
                break;
            }
            case Policy::Trig: {
                if (value == 0.f) break;
                auto cur = slot.load(std::memory_order_relaxed);
                auto next = Slot{};
                do {
                    next = Slot{value, cur.count + 1};
                } while (!slot.compare_exchange_weak(cur, next,
                             std::memory_order_release, std::memory_order_relaxed));
                break;
            }
            default:
                break;
        }
    }

    // Fill `out` with one sample per address. One pass, nothing to drain, and
    // nothing to run dry: a reader that skipped a thousand blocks gets the same
    // answer shape as one that skipped none.
    //
    // [consumer thread]
    auto read(std::span<Sample> out) -> void
    {
        for (auto i = uint32_t{}; i < num_meters; ++i) {
            auto& slot = _slots[i];

            switch (User_meters::spec(i).policy) {
                case Policy::Stream: {
                    out[i] = Sample{slot.load(std::memory_order_acquire).value, 0};
                    break;
                }
                case Policy::Peak: {
                    // Take the measurement and the count that produced it in one
                    // step, and put back only the cleared value — the count stays
                    // monotonic so "nothing arrived" is still distinguishable from
                    // "a measurement of zero".
                    auto cur = slot.load(std::memory_order_acquire);
                    while (!slot.compare_exchange_weak(cur, Slot{0.f, cur.count},
                               std::memory_order_acq_rel, std::memory_order_acquire)) {}
                    if (cur.count != _seen[i]) {
                        _seen[i] = cur.count;
                        _held[i] = cur.value; // Legitimately zero when the signal stopped.
                    }
                    // Otherwise nothing was posted since the last read, which is not
                    // the same as a measurement of zero — keep showing the last one.
                    out[i] = Sample{_held[i], 0};
                    break;
                }
                case Policy::Trig: {
                    // Magnitude and count come from the same load, so they cannot
                    // disagree about which trigger is being described.
                    const auto cur = slot.load(std::memory_order_acquire);
                    // Unsigned subtraction: wrap is well defined and still yields the
                    // number of triggers since the last read.
                    out[i] = Sample{cur.value, cur.count - _seen[i]};
                    _seen[i] = cur.count;
                    break;
                }
                default: {
                    out[i] = Sample{};
                    break;
                }
            }
        }
    }

    // Drop whatever accumulated while nobody was reading. A window opening now
    // generally wants the world as it is rather than a peak held over from whenever
    // it was last closed, and a trigger backlog it could not have seen. Levels are
    // deliberately left alone — those the new reader does want.
    //
    // [consumer thread]
    auto discard() -> void
    {
        for (auto i = uint32_t{}; i < num_meters; ++i) {
            switch (User_meters::spec(i).policy) {
                case Policy::Peak: {
                    auto cur = _slots[i].load(std::memory_order_acquire);
                    while (!_slots[i].compare_exchange_weak(cur, Slot{0.f, cur.count},
                               std::memory_order_acq_rel, std::memory_order_acquire)) {}
                    _seen[i] = cur.count;
                    _held[i] = 0.f;
                    break;
                }
                case Policy::Trig:
                    _seen[i] = _slots[i].load(std::memory_order_acquire).count;
                    break;
                default:
                    break;
            }
        }
    }

private:

    // One atomic, not two. The value and the fact that it was published have to move
    // together: a reader that takes one without the other can drop a peak outright —
    // exchange the value out, see a count the producer has not bumped yet, conclude
    // nothing arrived, and discard what it just removed from the slot. Two atomics
    // leave that window open however they are ordered.
    //
    // Following `Change_list`, the pair is a single lock-free atomic. Both asserts are
    // load-bearing: compare_exchange compares the object representation, so a padding
    // bit would make it fail spuriously and spin.
    struct Slot {
        float value{};
        uint32_t count{};
    };
    static_assert(std::atomic<Slot>::is_always_lock_free);
    static_assert(sizeof(Slot) == sizeof(float) + sizeof(uint32_t)); // No padding bits.

    std::array<std::atomic<Slot>, num_meters> _slots{};
    std::array<uint32_t, num_meters> _seen{}; // Consumer-only: last count observed.
    std::array<float, num_meters> _held{};    // Consumer-only: last peak actually delivered.

};

} // namespace tiny::meters
