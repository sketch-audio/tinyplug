#include "vst3_messaging.h"

#include "pluginterfaces/vst/ivstattributes.h"

namespace tiny::vst3 {

namespace {

constexpr auto k_tag_attr = "tag";
constexpr auto k_bytes_attr = "bytes";

} // namespace

auto Message_router::dispatch(Steinberg::Vst::IMessage* msg) -> bool
{
    if (!msg) return false;

    const auto* id = msg->getMessageID();
    if (!id) return false;

    const auto it = _handlers.find(id);
    if (it == _handlers.end()) return false;

    auto* attrs = msg->getAttributes();
    if (!attrs) return false;

    auto tag = Steinberg::int64{};
    attrs->getInt(k_tag_attr, tag); // tolerate missing tag (defaults to 0).

    const void* data = nullptr;
    auto size = Steinberg::uint32{};
    attrs->getBinary(k_bytes_attr, data, size); // tolerate missing payload (zero-size).

    it->second(
        std::span<const std::byte>{static_cast<const std::byte*>(data), size},
        static_cast<uint32_t>(tag)
    );

    return true;
}

auto Message_sender::send(const char* id, std::span<const std::byte> bytes, uint32_t tag) -> bool
{
    if (!_owner) return false;

    auto* msg = _owner->allocateMessage();
    if (!msg) return false;

    msg->setMessageID(id);

    auto* attrs = msg->getAttributes();
    if (!attrs) {
        msg->release();
        return false;
    }

    attrs->setInt(k_tag_attr, static_cast<Steinberg::int64>(tag));
    if (!bytes.empty()) {
        attrs->setBinary(k_bytes_attr, bytes.data(), static_cast<Steinberg::uint32>(bytes.size()));
    }

    const auto result = _owner->sendMessage(msg);
    msg->release();

    return result == Steinberg::kResultOk;
}

} // namespace tiny::vst3
