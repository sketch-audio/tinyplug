#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "tiny_log.hpp"

namespace tiny::state {

static_assert(std::endian::native == std::endian::little, "State records are written little-endian by memcpy.");

// Values a payload may hold: fixed-size bytes with nothing to follow. A nested struct is
// allowed, but then its layout is part of the format.
template<typename V>
concept Payload_value = std::is_trivially_copyable_v<V>
    && !std::is_pointer_v<V>
    && !std::is_member_pointer_v<V>;

// Owns a payload being written. `Writer` is the author's handle to it.
class Payload_out {
public:

    class Writer {
    public:
        explicit Writer(Payload_out* receiver = nullptr) : _receiver{receiver} {}

        template<Payload_value V>
        auto write(const V& value) const -> bool { return _receiver->write(value); }

    private:
        Payload_out* _receiver{nullptr};
    };

    auto writer() -> Writer { return Writer{this}; }

    template<Payload_value V>
    auto write(const V& value) -> bool
    {
        const auto* p = reinterpret_cast<const std::byte*>(&value);
        _bytes.insert(_bytes.end(), p, p + sizeof(V));
        return true;
    }

    auto bytes() const -> std::span<const std::byte> { return _bytes; }
    auto take() -> std::vector<std::byte> { return std::move(_bytes); }

private:

    std::vector<std::byte> _bytes{};

};

// Owns the read position in a payload. `Reader` is the author's handle to it. Bounds-checked:
// a read past the end fails, and every read after a failure fails too.
class Payload_in {
public:

    class Reader {
    public:
        explicit Reader(Payload_in* receiver = nullptr) : _receiver{receiver} {}

        template<Payload_value V>
        auto read(V& value) const -> bool { return _receiver->read(value); }

        // True for a record written without `save`: the payload is the bytes of an older `T`.
        auto raw() const -> bool { return _receiver->raw(); }

    private:
        Payload_in* _receiver{nullptr};
    };

    Payload_in(std::span<const std::byte> bytes, bool raw) : _bytes{bytes}, _raw{raw} {}

    auto reader() -> Reader { return Reader{this}; }

    template<Payload_value V>
    auto read(V& value) -> bool
    {
        if (_failed || remaining() < sizeof(V)) {
            _failed = true;
            return false;
        }
        std::memcpy(&value, _bytes.data() + _pos, sizeof(V));
        _pos += sizeof(V);
        return true;
    }

    auto raw() const -> bool { return _raw; }
    auto remaining() const -> std::size_t { return _bytes.size() - _pos; }
    auto failed() const -> bool { return _failed; }

private:

    std::span<const std::byte> _bytes{};
    std::size_t _pos{};
    bool _raw{};
    bool _failed{};

};

using Writer = Payload_out::Writer;
using Reader = Payload_in::Reader;

template<typename T>
concept Has_save = requires(Writer w, const T& v) { { T::save(w, v) } -> std::same_as<bool>; };

template<typename T>
concept Has_load = requires(Reader r, T& v) { { T::load(r, v) } -> std::same_as<bool>; };

// MARK: - record

// u32 magic, u32 record_version, u32 kind, u32 payload_length, then the payload.
struct Record_header {
    std::uint32_t magic{};
    std::uint32_t record_version{};
    std::uint32_t kind{};
    std::uint32_t payload_length{};
};
static_assert(sizeof(Record_header) == 16);

inline constexpr auto record_magic = std::uint32_t{'tSTA'};
inline constexpr auto record_version = std::uint32_t{1};

enum class Record_kind : std::uint32_t { Functions = 0, Raw = 1 };

namespace detail {

template<typename T>
auto check_pair() -> void
{
    static_assert(Has_save<T> == Has_load<T>,
        "Declare `static auto save(state::Writer, const T&) -> bool` and "
        "`static auto load(state::Reader, T&) -> bool` together, or neither.");
}

template<typename T>
auto save_payload(const T& value, Payload_out& out) -> bool
{
    return T::save(out.writer(), value);
}

// Load into a default `T`; only a clean, complete load is returned.
template<typename T>
auto load_payload(std::span<const std::byte> payload, bool raw, T& out) -> bool
{
    auto scratch = T{};
    auto in = Payload_in{payload, raw};
    if (!T::load(in.reader(), scratch) || in.failed()) return false;
    assert(in.remaining() == 0 && "state load left payload bytes unread; save and load disagree.");
    out = scratch;
    return true;
}

} // namespace detail

// The whole record for `value`. Off the audio thread; allocates.
template<typename T>
auto encode_record(const T& value) -> std::vector<std::byte>
{
    detail::check_pair<T>();

    auto payload = Payload_out{};
    payload.write(Record_header{}); // Filled in below, once the length is known.
    auto kind = Record_kind::Raw;

    if constexpr (Has_save<T>) {
        kind = Record_kind::Functions;
        [[maybe_unused]] const auto saved = detail::save_payload(value, payload);
        assert(saved && "state save returned false.");

#ifndef NDEBUG
        // Round trip: a field saved but not loaded, or loaded out of order, shows up here.
        const auto written = payload.bytes().subspan(sizeof(Record_header));
        auto reloaded = T{};
        const auto loaded = detail::load_payload(written, false, reloaded);
        assert(loaded && "state load refused what save just wrote.");
        auto again = Payload_out{};
        detail::save_payload(reloaded, again);
        assert(std::ranges::equal(written, again.bytes())
            && "state save -> load -> save changed the payload; save and load disagree.");
#endif
    }
    else {
        payload.write(value);
    }

    auto out = payload.take();

    const auto header = Record_header{
        record_magic,
        record_version,
        static_cast<std::uint32_t>(kind),
        static_cast<std::uint32_t>(out.size() - sizeof(Record_header))
    };
    std::memcpy(out.data(), &header, sizeof header);
    return out;
}

// Read a record into `out`. False leaves `out` untouched: nothing there, a foreign or
// future container, or a payload the author's `load` refused. Trailing bytes are ignored.
template<typename T>
auto decode_record(std::span<const std::byte> bytes, T& out) -> bool
{
    detail::check_pair<T>();

    if (bytes.empty()) return false; // A session from before the document existed.

    auto h = Record_header{};
    if (bytes.size() < sizeof h) {
        TINY_LOG_WARN(state, "state record truncated: {} bytes", bytes.size());
        return false;
    }
    std::memcpy(&h, bytes.data(), sizeof h);

    if (h.magic != record_magic || h.record_version != record_version) {
        TINY_LOG_WARN(state, "state record unreadable: magic={} version={}", h.magic, h.record_version);
        return false;
    }
    if (h.payload_length > bytes.size() - sizeof h) {
        TINY_LOG_WARN(state, "state record truncated: payload {} of {} bytes", bytes.size() - sizeof h, h.payload_length);
        return false;
    }

    const auto payload = bytes.subspan(sizeof h, h.payload_length);
    const auto raw = (h.kind == static_cast<std::uint32_t>(Record_kind::Raw));
    if (!raw && h.kind != static_cast<std::uint32_t>(Record_kind::Functions)) {
        TINY_LOG_WARN(state, "state record has unknown kind {}", h.kind);
        return false;
    }

    if constexpr (Has_load<T>) {
        if (detail::load_payload(payload, raw, out)) return true;
        TINY_LOG_WARN(state, "state load refused a {} byte payload (raw={})", payload.size(), raw);
        return false;
    }
    else {
        if (raw && payload.size() == sizeof(T)) {
            std::memcpy(&out, payload.data(), sizeof(T));
            return true;
        }
        TINY_LOG_WARN(state, "state record needs load(): kind={} payload {} bytes, sizeof(T) {}", h.kind, payload.size(), sizeof(T));
        return false;
    }
}

// The whole record's size from its first `sizeof(Record_header)` bytes, so a stream reader knows
// how much to read. Nullopt when the bytes are not a record this build understands.
inline auto record_size(std::span<const std::byte> header) -> std::optional<std::size_t>
{
    auto h = Record_header{};
    if (header.size() < sizeof h) return std::nullopt;
    std::memcpy(&h, header.data(), sizeof h);
    if (h.magic != record_magic || h.record_version != record_version) return std::nullopt;
    return sizeof h + std::size_t{h.payload_length};
}

// A record from a stream, where `read(std::byte*, size) -> bool` fills exactly `size` bytes. Empty
// when the stream ends first or holds something else. Read in slices, so a corrupt length can't
// allocate more than the stream actually delivers.
template<typename Read>
auto read_record(Read&& read) -> std::vector<std::byte>
{
    auto out = std::vector<std::byte>(sizeof(Record_header));
    if (!read(out.data(), out.size())) return {}; // Nothing after: a session from before the document.

    const auto size = record_size(out);
    if (!size) {
        TINY_LOG_WARN(state, "bytes after the chunk are not a state record");
        return {};
    }

    constexpr auto slice = std::size_t{64 * 1024};
    while (out.size() < *size) {
        const auto at = out.size();
        const auto want = std::min(slice, *size - at);
        out.resize(at + want);
        if (!read(out.data() + at, want)) {
            TINY_LOG_WARN(state, "state record truncated: {} of {} bytes", at, *size);
            return {};
        }
    }
    return out;
}

// The document a session should hold: its record, or the default when there is none.
template<typename T>
auto decode_record_or_default(std::span<const std::byte> bytes) -> T
{
    auto out = T{};
    if (!decode_record(bytes, out)) out = T{};
    return out;
}

} // namespace tiny::state
