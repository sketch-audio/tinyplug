#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/vstcomponentbase.h"

namespace tiny::vst3 {

// Reserved message-ID namespace.
//   tiny/worker/inbound       — controller → processor
//   tiny/worker/to_editor     — processor → controller
//   tiny/tables/<addr>        — future (controller → processor)
//   tiny/blocks/<addr>        — future (processor → controller)
//   tiny/<plugin>/<custom>    — reserved for plug-in-specific traffic
// All IDs should start with "tiny/" to avoid collisions with host-defined IDs.

// MARK: - router

// Dispatches incoming VST3 messages to registered handlers based on the
// message ID. The receiving wrapper (Vst3_processor or Vst3_controller) holds
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

// MARK: - shuttle

// Dedicated non-realtime polling thread that drains caller-supplied
// queues and forwards their contents over IMessage. Owned by the
// processor side; lets the audio thread push lock-free into an SPSC
// queue while all IMessage allocation/send happens off the audio
// thread. Replaces our (failed) attempt to use the SDK's
// DataExchangeHandler whose fallback path was unreliable in Bitwig and
// Live 11.
class Outbound_message_shuttle {
public:

    using Drain_fn = std::function<void()>;

    Outbound_message_shuttle() = default;
    ~Outbound_message_shuttle() { stop(); }

    Outbound_message_shuttle(const Outbound_message_shuttle&) = delete;
    auto operator=(const Outbound_message_shuttle&) -> Outbound_message_shuttle& = delete;
    Outbound_message_shuttle(Outbound_message_shuttle&&) = delete;
    auto operator=(Outbound_message_shuttle&&) -> Outbound_message_shuttle& = delete;

    auto register_drain(Drain_fn drain) -> void { _drains.push_back(std::move(drain)); }

    auto start(std::chrono::milliseconds poll_interval) -> void
    {
        if (_running.exchange(true, std::memory_order_acq_rel)) return; // Already running.
        _poll = poll_interval;
        _thread = std::thread([this]() {
            while (_running.load(std::memory_order_acquire)) {
                for (const auto& drain : _drains) drain();
                std::this_thread::sleep_for(_poll);
            }
        });
    }

    auto stop() -> void
    {
        if (!_running.exchange(false, std::memory_order_acq_rel)) return;
        if (_thread.joinable()) _thread.join();
    }

private:

    std::vector<Drain_fn> _drains{};
    std::thread _thread{};
    std::atomic<bool> _running{false};
    std::chrono::milliseconds _poll{16};

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
