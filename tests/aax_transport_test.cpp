// AAX's private-data transports: Byte_ring and Block_store. Both are standard-library only, so
// no SDK. The remote end (Direct Data) reaches them by byte offset through Read/WritePortDirect;
// the remote readers here follow direct_data.cpp's protocol over the same offsets.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>

#include "block_store.hpp"
#include "byte_ring.hpp"

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define TINY_TEST_TSAN 1
#  endif
#endif

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::aax::Block_store;
using tiny::aax::Byte_ring;
using tiny::aax::Ring_kind;

// A payload of `n` bytes that identifies itself: byte i is (seq + i).
auto pattern(uint32_t seq, uint32_t n) -> std::vector<unsigned char>
{
    auto out = std::vector<unsigned char>(n);
    for (auto i = uint32_t{}; i < n; ++i) out[i] = static_cast<unsigned char>(seq + i);
    return out;
}

auto matches(const void* data, uint32_t seq, uint32_t n) -> bool
{
    return std::memcmp(data, pattern(seq, n).data(), n) == 0;
}

// Sizes chosen so entries straddle the end of the storage on most trips round it.
constexpr auto size_for(uint32_t seq) -> uint32_t { return 1 + (seq * 7) % 40; }

// MARK: - Byte_ring

auto add_byte_ring() -> void
{
    Tests::add("Byte_ring: entries round-trip with kind, size and bytes, in order", [] {
        auto ring = std::make_unique<Byte_ring<256>>();
        for (auto seq = uint32_t{}; seq < 5; ++seq) {
            const auto bytes = pattern(seq, size_for(seq));
            expect_true(ring->push(Ring_kind::Worker_from_processor, bytes.data(), size_for(seq)), "push");
        }
        auto seq = uint32_t{};
        ring->drain([&](Ring_kind kind, const void* payload, uint32_t n) {
            expect_true(kind == Ring_kind::Worker_from_processor, "kind");
            expect_true(n == size_for(seq) && matches(payload, seq, n), std::format("entry {} corrupted", seq));
            ++seq;
        });
        expect_true(seq == 5, "entries lost");
        expect_true(ring->read_pos.load() == ring->write_pos.load(), "drain must reclaim everything it read");
    });

    Tests::add("Byte_ring: a full ring refuses and writes nothing", [] {
        auto ring = std::make_unique<Byte_ring<64>>();
        const auto meter = tiny::aax::Ring_meter{.address = 1, .value = 0.5};
        auto pushed = 0;
        while (ring->push_value(Ring_kind::Meter, meter)) ++pushed; // 24 bytes each.
        expect_true(pushed == 2, std::format("{} meters fit in 64 bytes", pushed));
        const auto before = ring->write_pos.load();
        expect_true(!ring->push_value(Ring_kind::Meter, meter) && ring->write_pos.load() == before, "a refused push moved write_pos");
        auto drained = 0;
        ring->drain([&](Ring_kind, const void*, uint32_t) { ++drained; });
        expect_true(drained == 2, "drain count");
        expect_true(ring->push_value(Ring_kind::Meter, meter), "space returns after a drain");
    });

    Tests::add("Byte_ring: entries straddling the wrap survive many trips", [] {
        auto ring = std::make_unique<Byte_ring<64>>();
        auto next_out = uint32_t{};
        for (auto seq = uint32_t{}; seq < 2000; ++seq) {
            const auto bytes = pattern(seq, size_for(seq));
            if (!ring->push(Ring_kind::Editor_note, bytes.data(), size_for(seq))) {
                ring->drain([&](Ring_kind, const void* payload, uint32_t n) {
                    expect_true(n == size_for(next_out) && matches(payload, next_out, n), std::format("entry {} corrupted", next_out));
                    ++next_out;
                });
                expect_true(ring->push(Ring_kind::Editor_note, bytes.data(), size_for(seq)), "push after drain");
            }
        }
        ring->drain([&](Ring_kind, const void*, uint32_t) { ++next_out; });
        expect_true(next_out == 2000, std::format("{} of 2000 came out", next_out));
    });

    Tests::add("Byte_ring: an oversized entry makes drain reclaim everything rather than spin", [] {
        auto ring = std::make_unique<Byte_ring<2048>>();
        const auto big = std::vector<unsigned char>(Byte_ring<2048>::max_payload_bytes + 8);
        expect_true(ring->push(Ring_kind::Meter, big.data(), static_cast<uint32_t>(big.size())), "push");
        auto called = false;
        ring->drain([&](Ring_kind, const void*, uint32_t) { called = true; });
        expect_true(!called && ring->read_pos.load() == ring->write_pos.load(), "must skip and reclaim");
    });

    // What Direct Data does: read the head by offset, copy committed bytes, advance read_pos.
    Tests::add("Byte_ring: a remote reader by byte offset sees what the local producer wrote", [] {
        using Ring = Byte_ring<128>;
        auto ring = std::make_unique<Ring>();
        const auto* base = reinterpret_cast<const unsigned char*>(ring.get());
        auto next_out = uint32_t{};
        for (auto seq = uint32_t{}; seq < 500; ++seq) {
            const auto bytes = pattern(seq, size_for(seq));
            if (ring->push(Ring_kind::Worker_from_processor, bytes.data(), size_for(seq))) continue;

            auto head = std::array<uint64_t, 2>{};
            std::memcpy(head.data(), base + Ring::offset_write_pos, sizeof(head));
            auto offset = head[1];
            while (offset < head[0]) {
                auto header = tiny::aax::Ring_header{};
                auto copy = [&](uint64_t pos, void* dst, size_t n) {
                    for (auto i = size_t{}; i < n; ++i) static_cast<unsigned char*>(dst)[i] = base[Ring::offset_data + ((pos + i) & Ring::mask)];
                };
                copy(offset, &header, sizeof(header));
                auto payload = std::vector<unsigned char>(header.payload_bytes);
                copy(offset + sizeof(header), payload.data(), header.payload_bytes);
                expect_true(header.payload_bytes == size_for(next_out) && matches(payload.data(), next_out, header.payload_bytes), "remote read corrupted");
                ++next_out;
                offset += sizeof(header) + tiny::aax::ring_align_up(header.payload_bytes);
            }
            ring->read_pos.store(offset); // WritePortDirect of read_pos.
            expect_true(ring->push(Ring_kind::Worker_from_processor, bytes.data(), size_for(seq)), "push after remote read");
        }
        expect_true(next_out > 400, "the remote reader should have read most entries");
    });

    Tests::add("Byte_ring: producer and consumer threads, nothing lost or corrupted", [] {
        auto ring = std::make_unique<Byte_ring<1024>>();
        constexpr auto n = uint32_t{100'000};
        auto producer = std::thread{[&] {
            for (auto seq = uint32_t{}; seq < n; ++seq) {
                const auto bytes = pattern(seq, size_for(seq));
                while (!ring->push(Ring_kind::Worker_to_processor, bytes.data(), size_for(seq))) std::this_thread::yield();
            }
        }};
        auto next_out = uint32_t{};
        auto bad = uint32_t{};
        while (next_out < n) {
            ring->drain([&](Ring_kind, const void* payload, uint32_t size) {
                if (size != size_for(next_out) || !matches(payload, next_out, size)) ++bad;
                ++next_out;
            });
        }
        producer.join();
        expect_true(bad == 0, std::format("{} corrupted entries", bad));
    });
}

// MARK: - Block_store

struct Frame {
    std::array<uint64_t, 32> words{}; // Uniform when intact, so a torn copy is detectable.
};

// Direct Data's reader: seq, the front slot, seq again; accept only if seq held still.
template<typename Store>
auto remote_read(const Store& store, typename Store::Frame_type& out, uint64_t& seq) -> bool
{
    const auto* base = reinterpret_cast<const unsigned char*>(&store);
    const auto before = store.seq.load(std::memory_order_acquire);
    if (before == 0) return false;
    std::memcpy(&out, base + Store::offset_front(before), Store::frame_bytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto after = store.seq.load(std::memory_order_relaxed);
    seq = before;
    return before == after;
}

auto frame_of(uint64_t value) -> Frame
{
    auto f = Frame{};
    f.words.fill(value);
    return f;
}

auto add_block_store() -> void
{
    static_assert(tiny::aax::block_store_layout_ok<Frame>);

    Tests::add("Block_store: nothing published reads as nothing, not a zero frame", [] {
        auto store = std::make_unique<Block_store<Frame>>();
        auto out = Frame{};
        auto seq = uint64_t{};
        expect_true(!remote_read(*store, out, seq), "seq 0 must not be forwarded");
    });

    Tests::add("Block_store: the reader always gets the latest publish", [] {
        auto store = std::make_unique<Block_store<Frame>>();
        for (auto k = uint64_t{1}; k <= 5; ++k) {
            store->publish(frame_of(k));
            auto out = Frame{};
            auto seq = uint64_t{};
            expect_true(remote_read(*store, out, seq) && out.words[0] == k && out.words[31] == k, std::format("publish {}", k));
        }
    });

    // The interleavings that matter, stepped by hand.
    Tests::add("Block_store: a publish during the copy is rejected; the next read recovers", [] {
        auto store = std::make_unique<Block_store<Frame>>();
        store->publish(frame_of(1));
        const auto before = store->seq.load();
        store->publish(frame_of(2)); // Lands between the reader's two seq reads.
        store->publish(frame_of(3)); // This one overwrites the slot the reader was copying.
        expect_true(store->seq.load() != before, "the reader's recheck must fail");
        auto out = Frame{};
        auto seq = uint64_t{};
        expect_true(remote_read(*store, out, seq) && out.words[0] == 3, "a fresh read gets the newest frame");
    });

    Tests::add("Block_store: publish_with fills the back slot in place", [] {
        auto store = std::make_unique<Block_store<Frame>>();
        store->publish(frame_of(7));
        store->publish_with([](unsigned char* slot) { auto f = frame_of(8); std::memcpy(slot, &f, sizeof(f)); });
        auto out = Frame{};
        auto seq = uint64_t{};
        expect_true(remote_read(*store, out, seq) && out.words[5] == 8 && seq == 2, "publish_with");
    });

#ifndef TINY_TEST_TSAN
    // A seqlock's copy races with the writer by design (the real reader is the host's memcpy),
    // so this runs outside TSan only: under load, no accepted frame may ever be torn.
    Tests::add("Block_store: under a fast publisher, accepted frames are never torn", [] {
        auto store = std::make_unique<Block_store<Frame>>();
        auto stop = std::atomic<bool>{};
        auto publisher = std::thread{[&] {
            for (auto k = uint64_t{1}; !stop.load(std::memory_order_relaxed); ++k) store->publish(frame_of(k));
        }};
        auto accepted = 0, torn = 0;
        for (auto i = 0; i < 200'000; ++i) {
            auto out = Frame{};
            auto seq = uint64_t{};
            if (!remote_read(*store, out, seq)) continue;
            ++accepted;
            for (const auto w : out.words) if (w != out.words[0]) { ++torn; break; }
        }
        stop = true;
        publisher.join();
        expect_true(torn == 0, std::format("{} torn frames accepted", torn));
        expect_true(accepted > 0, "the reader never got a frame");
    });
#endif
}

} // namespace

auto main() -> int
{
    add_byte_ring();
    add_block_store();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
