#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "state_record.hpp"
#include "tiny_input.hpp" // Rect_size

namespace tiny {

// The editor's side of a session, which the framework owns: the window size and the view model's
// two parts, each a state record. One blob in every format, beside the document's record: appended
// to a positional chunk (the CLAP stream, the VST3 controller chunk), under `tinyplug-edit` in the
// AU and AAX containers, `"edit"` in a web session. Framed like a state record, with its own magic:
//
//     u32 'tEDT', u32 version, u32 kind (0), u32 payload length,
//     then i32 width, i32 height (0 for none), u32 n, n bytes (Undoable), u32 m, m bytes (Restored).
struct Edit_record {
    std::optional<Rect_size> size{};
    std::vector<std::byte> undoable{};
    std::vector<std::byte> restored{};

    auto operator==(const Edit_record&) const -> bool = default;
};

inline constexpr auto edit_record_magic = std::uint32_t{'tEDT'};
inline constexpr auto edit_record_version = std::uint32_t{1};

namespace detail {

inline auto put(std::vector<std::byte>& out, const void* data, std::size_t n) -> void
{
    const auto* p = static_cast<const std::byte*>(data);
    out.insert(out.end(), p, p + n);
}

template<typename T>
auto get(std::span<const std::byte>& in, T& out) -> bool
{
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&out, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}

inline auto get_bytes(std::span<const std::byte>& in, std::vector<std::byte>& out) -> bool
{
    auto n = std::uint32_t{};
    if (!get(in, n) || n > in.size()) return false;
    out.assign(in.begin(), in.begin() + n);
    in = in.subspan(n);
    return true;
}

// A framed record's whole size from its header, when its magic is `magic`.
inline auto framed_size(std::span<const std::byte> header, std::uint32_t magic) -> std::optional<std::size_t>
{
    auto h = state::Record_header{};
    if (header.size() < sizeof h) return std::nullopt;
    std::memcpy(&h, header.data(), sizeof h);
    if (h.magic != magic) return std::nullopt;
    const auto total = std::uint64_t{sizeof h} + h.payload_length;
    if (total > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    return static_cast<std::size_t>(total);
}

} // namespace detail

inline auto encode_edit_record(const Edit_record& record) -> std::vector<std::byte>
{
    auto payload = std::vector<std::byte>{};
    const auto w = record.size ? record.size->w : 0;
    const auto h = record.size ? record.size->h : 0;
    detail::put(payload, &w, sizeof w);
    detail::put(payload, &h, sizeof h);
    for (const auto* part : {&record.undoable, &record.restored}) {
        const auto n = static_cast<std::uint32_t>(part->size());
        detail::put(payload, &n, sizeof n);
        detail::put(payload, part->data(), part->size());
    }

    const auto header = state::Record_header{edit_record_magic, edit_record_version, 0, static_cast<std::uint32_t>(payload.size())};
    auto out = std::vector<std::byte>{};
    detail::put(out, &header, sizeof header);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Nullopt for anything that isn't an edit record this build reads, from a newer build included.
inline auto decode_edit_record(std::span<const std::byte> bytes) -> std::optional<Edit_record>
{
    const auto total = detail::framed_size(bytes, edit_record_magic);
    if (!total || *total > bytes.size()) return std::nullopt;
    auto header = state::Record_header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (header.record_version != edit_record_version) return std::nullopt;

    auto in = bytes.subspan(sizeof header, header.payload_length);
    auto record = Edit_record{};
    auto w = int32_t{}, h = int32_t{};
    if (!detail::get(in, w) || !detail::get(in, h)) return std::nullopt;
    if (w > 0 && h > 0) record.size = Rect_size{w, h};
    if (!detail::get_bytes(in, record.undoable) || !detail::get_bytes(in, record.restored)) return std::nullopt;
    return record;
}

// The records after a positional chunk (the CLAP stream, a VST3 chunk), in either order: the
// document's and the editor's. Reading stops at the end of the stream or at anything else, so an
// older session, with neither or only the first, reads as it always did.
struct Trailing_records {
    std::vector<std::byte> state{};
    std::vector<std::byte> edit{};
};

// `read(std::byte*, size) -> bool` fills exactly `size` bytes.
template<typename Read>
auto read_trailing_records(Read&& read) -> Trailing_records
{
    auto out = Trailing_records{};
    for (auto n = 0; n < 2; ++n) {
        auto bytes = std::vector<std::byte>(sizeof(state::Record_header));
        if (!read(bytes.data(), bytes.size())) break;

        auto* into = static_cast<std::vector<std::byte>*>(nullptr);
        auto size = std::optional<std::size_t>{};
        if ((size = detail::framed_size(bytes, state::record_magic))) into = &out.state;
        else if ((size = detail::framed_size(bytes, edit_record_magic))) into = &out.edit;
        if (!into || !into->empty()) break;

        constexpr auto slice = std::size_t{64 * 1024}; // A corrupt length allocates no more than arrives.
        auto ok = true;
        while (bytes.size() < *size) {
            const auto at = bytes.size();
            const auto want = std::min(slice, *size - at);
            bytes.resize(at + want);
            if (!read(bytes.data() + at, want)) { ok = false; break; }
        }
        if (!ok) break;
        *into = std::move(bytes);
    }
    return out;
}

} // namespace tiny
