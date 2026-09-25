#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/vstcomponentbase.h"

namespace tiny::vst3 {

// Reserved message-ID namespace.
//   tiny/worker/inbound       — controller → processor
//   tiny/worker/to_editor     — processor → controller
//   tiny/latency/changed      — processor → controller
//   tiny/tables/<addr>        — future (controller → processor)
//   tiny/blocks               — processor → controller, tag is the block address
//   tiny/state/edit           — controller → processor
//   tiny/state/snapshot       — processor → controller
//   tiny/<plugin>/<custom>    — reserved for plug-in-specific traffic
// All IDs should start with "tiny/" to avoid collisions with host-defined IDs.

// Payload is the new latency in samples (uint32_t). Not worker traffic, so it lives
// outside TINY_HAS_WORKER — every plug-in needs it.
inline constexpr auto k_latency_changed_id = "tiny/latency/changed";

// Payload is one block frame; the tag is its address.
inline constexpr auto k_blocks_id = "tiny/blocks";

// State document. Edit: controller -> processor, tag is the edit sequence. Snapshot:
// processor -> controller, tag is the processor's edit generation.
inline constexpr auto k_state_edit_id = "tiny/state/edit";
inline constexpr auto k_state_snapshot_id = "tiny/state/snapshot";
inline constexpr auto k_notes_id = "tiny/notes";                   // controller → processor: one `midi::Performance`

// MARK: - router

// Dispatches incoming VST3 messages to registered handlers based on the
// message ID. The receiving wrapper (Audio_effect or Controller) holds
// one and calls `dispatch` from its `notify()` override.
class Message_router {
public:

    // Callback signature: payload bytes + alternative-index tag.
    using Receive_fn = std::function<void(std::span<const std::byte>, uint32_t alt_index)>;

    auto register_handler(const char* id, Receive_fn fn) -> void
    {
        _handlers[id] = std::move(fn);
    }

    auto unregister_handler(const char* id) -> void
    {
        _handlers.erase(id);
    }

    // Returns true if message was recognized and dispatched.
    auto dispatch(Steinberg::Vst::IMessage* msg) -> bool;

private:

    std::unordered_map<std::string, Receive_fn> _handlers{};

};

// MARK: - full reads

// IBStream::read may return fewer bytes than asked, and IBStreamer (and a single `read` call) treat
// a short read as a failed load. Wraps a host stream so every read loops until it has the bytes or
// the stream ends. A stack object: point the `state` parameter at it for the rest of the load.
class Full_read_stream final : public Steinberg::IBStream {
public:

    explicit Full_read_stream(Steinberg::IBStream* inner) : _inner{inner} {}

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 size, Steinberg::int32* bytes_read) SMTG_OVERRIDE
    {
        auto total = Steinberg::int32{};
        while (total < size) {
            auto got = Steinberg::int32{};
            if (_inner->read(static_cast<char*>(buffer) + total, size - total, &got) != Steinberg::kResultOk || got <= 0) break;
            total += got;
        }
        if (bytes_read) *bytes_read = total;
        return total > 0 || size == 0 ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 size, Steinberg::int32* written) SMTG_OVERRIDE { return _inner->write(buffer, size, written); }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) SMTG_OVERRIDE { return _inner->seek(pos, mode, result); }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) SMTG_OVERRIDE { return _inner->tell(pos); }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void** obj) SMTG_OVERRIDE { *obj = nullptr; return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }

private:

    Steinberg::IBStream* _inner{};

};

// MARK: - sender

// Wraps a ComponentBase's allocateMessage/sendMessage so subsystems can emit
// typed payloads to the peer without re-inventing the encoding each time.
class Message_sender {
public:

    Message_sender() = default;
    explicit Message_sender(Steinberg::Vst::ComponentBase* owner) : _owner{owner} {}

    auto set_owner(Steinberg::Vst::ComponentBase* owner) -> void { _owner = owner; }

    // Send raw bytes + a tag.
    auto send(const char* id, std::span<const std::byte> bytes, uint32_t tag = 0) -> bool;

    // Convenience for trivially-copyable POD payloads.
    template <typename T>
    auto send_pod(const char* id, const T& payload, uint32_t tag = 0) -> bool
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::byte*>(&payload);
        return send(id, std::span<const std::byte>{p, sizeof(T)}, tag);
    }

    // Convenience for std::variant<...> of trivially-copyable alternatives.
    // Degrades to a no-op when V is std::monostate (i.e. the user has no
    // worker, so the variant slot collapsed to monostate).
    template <typename V>
    auto send_variant(const char* id, const V& v) -> bool
    {
        if constexpr (std::is_same_v<V, std::monostate>) {
            (void)id; (void)v;
            return false;
        }
        else {
            return std::visit([&](const auto& alt) {
                using Alt = std::remove_cvref_t<decltype(alt)>;
                if constexpr (std::is_same_v<Alt, std::monostate>) {
                    // No payload for monostate; still emit with tag.
                    return send(id, std::span<const std::byte>{}, static_cast<uint32_t>(v.index()));
                }
                else {
                    return this->send_pod(id, alt, static_cast<uint32_t>(v.index()));
                }
            }, v);
        }
    }

private:

    Steinberg::Vst::ComponentBase* _owner{nullptr};

};

// MARK: - variant reconstruction

namespace impl {

template <typename V, size_t I>
auto try_emplace(V& v, std::span<const std::byte> bytes, uint32_t tag) -> bool
{
    if (tag != I) return false;
    using Alt = std::variant_alternative_t<I, V>;
    if constexpr (std::is_same_v<Alt, std::monostate>) {
        v.template emplace<I>();
        return true;
    }
    else {
        static_assert(std::is_trivially_copyable_v<Alt>, "Variant alternatives must be trivially copyable to use VST3 message bridge.");
        if (bytes.size() != sizeof(Alt)) return false;
        Alt a{};
        std::memcpy(&a, bytes.data(), sizeof(Alt));
        v.template emplace<I>(a);
        return true;
    }
}

template <typename V, size_t... I>
auto reconstruct_impl(std::span<const std::byte> bytes, uint32_t tag, std::index_sequence<I...>) -> V
{
    auto out = V{};
    (try_emplace<V, I>(out, bytes, tag) || ...);
    return out;
}

} // namespace impl

// Compile-time maximum sizeof across all alternatives of a variant.
// Used to size Data Exchange blocks: block must fit the largest alternative
// plus the tag.
namespace impl {

template <typename V, size_t... I>
constexpr auto max_alt_size_impl(std::index_sequence<I...>) -> uint32_t
{
    uint32_t out = 0;
    ((out = std::max<uint32_t>(out, sizeof(std::variant_alternative_t<I, V>))), ...);
    return out;
}

} // namespace impl

template <typename V>
constexpr auto max_alternative_size() -> uint32_t
{
    if constexpr (std::is_same_v<V, std::monostate>) {
        return 0;
    }
    else {
        return impl::max_alt_size_impl<V>(std::make_index_sequence<std::variant_size_v<V>>{});
    }
}

// Reconstruct a variant from its serialized bytes + alternative index tag.
// Degrades to a default-constructed V when V is std::monostate.
template <typename V>
auto reconstruct_variant(std::span<const std::byte> bytes, uint32_t tag) -> V
{
    if constexpr (std::is_same_v<V, std::monostate>) {
        (void)bytes; (void)tag;
        return V{};
    }
    else {
        return impl::reconstruct_impl<V>(bytes, tag, std::make_index_sequence<std::variant_size_v<V>>{});
    }
}

} // namespace tiny::vst3
