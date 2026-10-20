// The C++ end of wire_test.mjs: real wire::Byte_ring / wire::Block_store in a module's memory, with exports that
// produce and check self-identifying traffic. The test instantiates it twice, as the web format's two modules.
#include <array>
#include <cstdint>
#include <cstring>

#include <emscripten/emscripten.h>

#include <tiny_core/wire.hpp>

namespace {

using Ring = tiny::wire::Byte_ring<256>; // Small, so wraparound and a full ring happen constantly.
struct Frame { std::array<std::uint32_t, 8> words{}; };
using Store = tiny::wire::Block_store<Frame>;

Ring rings[2]{};
Store store{};
std::uint32_t next_push[2]{};
std::uint32_t next_expect[2]{};

// Entry `seq` carries `seq % 61` bytes, byte k = seq + k.
auto fill(std::uint32_t seq, unsigned char* out) -> std::uint32_t
{
    const auto n = seq % 61;
    for (auto k = std::uint32_t{}; k < n; ++k) out[k] = static_cast<unsigned char>(seq + k);
    return n;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE auto wire_ring(int i) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(&rings[i]); }
EMSCRIPTEN_KEEPALIVE auto wire_ring_capacity() -> std::uint32_t { return Ring::capacity; }
EMSCRIPTEN_KEEPALIVE auto wire_ring_data_offset() -> std::uint32_t { return Ring::offset_data; }
EMSCRIPTEN_KEEPALIVE auto wire_store() -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(&store); }
EMSCRIPTEN_KEEPALIVE auto wire_store_frame_bytes() -> std::uint32_t { return Store::frame_bytes; }
EMSCRIPTEN_KEEPALIVE auto wire_store_slot_offset() -> std::uint32_t { return Store::offset_slots; }

// Pushes up to `count` entries; returns how many fit.
EMSCRIPTEN_KEEPALIVE auto wire_push(int i, int count) -> int
{
    auto pushed = 0;
    for (; pushed < count; ++pushed) {
        auto payload = std::array<unsigned char, 64>{};
        const auto n = fill(next_push[i], payload.data());
        if (!rings[i].push(next_push[i], payload.data(), n)) break;
        ++next_push[i];
    }
    return pushed;
}

// Drains and checks every entry: consecutive kinds, the right size, the right bytes. -1 on any mismatch.
EMSCRIPTEN_KEEPALIVE auto wire_consume(int i) -> int
{
    auto consumed = 0;
    auto ok = true;
    rings[i].drain([&](std::uint32_t kind, const void* payload, std::uint32_t bytes) {
        auto expected = std::array<unsigned char, 64>{};
        const auto n = fill(next_expect[i], expected.data());
        ok = ok && kind == next_expect[i] && bytes == n && std::memcmp(payload, expected.data(), n) == 0;
        ++next_expect[i];
        ++consumed;
    });
    return ok ? consumed : -1;
}

EMSCRIPTEN_KEEPALIVE auto wire_publish(std::uint32_t value) -> void
{
    auto frame = Frame{};
    frame.words.fill(value);
    store.publish(frame);
}

// The reader protocol on the C++ side: the latest value, 0 if never published, -1 if a publish overlapped the copy
// (retry: the protocol working), -2 if a copy that passed the seq check is inconsistent (the protocol failing).
EMSCRIPTEN_KEEPALIVE auto wire_store_value() -> std::int64_t
{
    const auto s = store.seq.load(std::memory_order_acquire);
    if (s == 0) return 0;
    auto frame = Frame{};
    std::memcpy(&frame, store.slots + (s - 1 & 1) * sizeof(Frame), sizeof(Frame));
    if (store.seq.load(std::memory_order_acquire) != s) return -1;
    for (const auto w : frame.words) if (w != frame.words[0]) return -2;
    return frame.words[0];
}

} // extern "C"
