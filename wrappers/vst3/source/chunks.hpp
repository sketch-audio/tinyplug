#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"

#include <tiny_core/state_adapter.hpp> // State_map, State_tag
#include <tiny_core/state_rules.hpp>

#include "plug_info.hpp"

// The VST3 chunks' layouts, written in one place for the wrapper and the preset exporter
// (tools/presets), which builds .vstpreset files without a plug-in instance.
namespace tiny::vst3 {

namespace chunk_detail {

inline auto write_bytes(Steinberg::IBStream* stream, std::span<const std::byte> bytes) -> bool
{
    if (bytes.empty()) return true;
    auto written = Steinberg::int32{};
    return stream->write(const_cast<std::byte*>(bytes.data()), static_cast<Steinberg::int32>(bytes.size()), &written) == Steinberg::kResultOk
        && static_cast<size_t>(written) == bytes.size();
}

inline auto header(uint32_t count) -> State_rules::Vst3::Header
{
    return {Plug_info::framework_code, Plug_info::manufacturer_code, Plug_info::plugin_code, count};
}

} // namespace chunk_detail

// The processor's chunk: header, one knob-space float per parameter (`no_value` for one not saved),
// the bypass (`no_value`: no opinion), then the document's record when there is one.
inline auto write_processor_chunk(Steinberg::IBStream* stream, std::span<const float> knob_values, float bypass,
                                  std::span<const std::byte> record) -> bool
{
    auto streamer = Steinberg::IBStreamer{stream};
    const auto head = chunk_detail::header(static_cast<uint32_t>(knob_values.size()));
    if (!streamer.writeInt32uArray(head.data(), static_cast<int32_t>(head.size()))) return false;
    for (const auto value : knob_values) {
        if (!streamer.writeFloat(value)) return false;
    }
    if (!streamer.writeFloat(bypass)) return false;
    return chunk_detail::write_bytes(stream, record);
}

// The controller's chunk: header, the editor map's tagged pairs (the framework's keys, and in an older
// session or a factory preset the author's), then the editor's record when there is one.
inline auto write_controller_chunk(Steinberg::IBStream* stream, const State_map& pairs, std::span<const std::byte> edit_record) -> bool
{
    auto streamer = Steinberg::IBStreamer{stream};
    const auto head = chunk_detail::header(static_cast<uint32_t>(pairs.size()));
    if (!streamer.writeInt32uArray(head.data(), static_cast<int32_t>(head.size()))) return false;

    auto write_container = [&](const auto& container) {
        const auto num = static_cast<uint32_t>(container.size());
        if (!streamer.writeInt32u(num)) return false;
        return num == 0 || streamer.writeRaw(container.data(), static_cast<Steinberg::int32>(sizeof(container[0]) * num));
    };

    for (const auto& [key, value] : pairs) {
        if (!write_container(key)) return false;
        const auto tag = tag_for(value);
        if (!streamer.writeInt32u(static_cast<uint32_t>(tag))) return false;
        const auto ok = std::visit(Inline_visitor{
            [&](bool v) { return streamer.writeBool(v); },
            [&](int32_t v) { return streamer.writeInt32(v); },
            [&](double v) { return streamer.writeDouble(v); },
            [&](const std::string& v) { return write_container(v); },
        }, value);
        if (!ok) return false;
    }
    return chunk_detail::write_bytes(stream, edit_record);
}

} // namespace tiny::vst3
