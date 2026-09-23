#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <tiny_core/base64.hpp>
#include <tiny_core/state_record.hpp>

namespace {

namespace state = tiny::state;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

// MARK: - documents

// The shape an author ships first.
struct Doc_v1 {
    std::array<std::uint8_t, 8> notes{};

    static auto save(state::Writer out, const Doc_v1& value) -> bool
    {
        return out.write(std::uint32_t{1}) && out.write(value.notes);
    }

    static auto load(state::Reader in, Doc_v1& value) -> bool
    {
        auto version = std::uint32_t{};
        if (!in.read(version) || version > 1) return false;
        return in.read(value.notes);
    }
};

// The same document a release later: a field added in front, which a raw copy could not survive.
struct Doc_v2 {
    std::uint8_t swing{50};
    std::array<std::uint8_t, 8> notes{};

    static auto save(state::Writer out, const Doc_v2& value) -> bool
    {
        return out.write(std::uint32_t{2}) && out.write(value.notes) && out.write(value.swing);
    }

    static auto load(state::Reader in, Doc_v2& value) -> bool
    {
        auto version = std::uint32_t{};
        if (!in.read(version) || version > 2) return false;
        if (!in.read(value.notes)) return false;
        if (version >= 2 && !in.read(value.swing)) return false;
        return true;
    }
};

// A v3 an older build will meet in a newer session.
struct Doc_v3 {
    std::array<std::uint8_t, 8> notes{};
    std::uint8_t swing{};
    float tempo{};

    static auto save(state::Writer out, const Doc_v3& value) -> bool
    {
        return out.write(std::uint32_t{3}) && out.write(value.notes) && out.write(value.swing) && out.write(value.tempo);
    }

    static auto load(state::Reader in, Doc_v3& value) -> bool
    {
        auto version = std::uint32_t{};
        return in.read(version) && in.read(value.notes) && in.read(value.swing) && in.read(value.tempo);
    }
};

// No functions: the raw fallback.
struct Raw_doc {
    std::uint32_t a{7};
    float b{0.5f};
};

// The same document after the author adds functions, keeping the old layout readable.
struct Upgraded_doc {
    std::uint32_t a{7};
    float b{0.5f};
    std::uint16_t c{3};

    static auto save(state::Writer out, const Upgraded_doc& value) -> bool
    {
        return out.write(std::uint32_t{1}) && out.write(value.a) && out.write(value.b) && out.write(value.c);
    }

    static auto load(state::Reader in, Upgraded_doc& value) -> bool
    {
        if (in.raw()) {
            auto old = Raw_doc{};
            if (!in.read(old)) return false;
            value.a = old.a;
            value.b = old.b;
            return true;
        }
        auto version = std::uint32_t{};
        return in.read(version) && version == 1 && in.read(value.a) && in.read(value.b) && in.read(value.c);
    }
};

// A load that always refuses.
struct Refusing_doc {
    std::uint32_t x{42};

    static auto save(state::Writer out, const Refusing_doc& value) -> bool { return out.write(value.x); }
    static auto load(state::Reader, Refusing_doc&) -> bool { return false; }
};

// A load that reads half, then refuses: nothing it wrote may leak out.
struct Half_doc {
    std::uint32_t x{1};
    std::uint32_t y{2};

    static auto save(state::Writer out, const Half_doc& value) -> bool { return out.write(value.x) && out.write(value.y); }
    static auto load(state::Reader in, Half_doc& value) -> bool
    {
        return in.read(value.x) && value.x < 100 && in.read(value.y);
    }
};

static_assert(state::Has_save<Doc_v1> && state::Has_load<Doc_v1>);
static_assert(!state::Has_save<Raw_doc> && !state::Has_load<Raw_doc>);
static_assert(!state::Payload_value<int*>);
static_assert(state::Payload_value<std::array<float, 4>>);

// A record built by hand, for payloads `encode_record` would refuse to write in debug builds.
auto make_record(std::uint32_t kind, std::vector<std::uint32_t> words) -> std::vector<std::byte>
{
    const auto h = state::Record_header{state::record_magic, state::record_version, kind,
        static_cast<std::uint32_t>(words.size() * 4)};
    auto out = std::vector<std::byte>(sizeof h + words.size() * 4);
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, words.data(), words.size() * 4);
    return out;
}

template<typename T>
auto same(const T& a, const T& b) -> bool { return std::memcmp(&a, &b, sizeof(T)) == 0; }

// MARK: - tests

auto test_functions() -> void
{
    std::printf("save/load\n");

    auto doc = Doc_v2{};
    doc.swing = 66;
    doc.notes = {1, 2, 3, 4, 5, 6, 7, 8};

    const auto bytes = state::encode_record(doc);
    auto back = Doc_v2{};
    expect(state::decode_record<Doc_v2>(bytes, back) && same(back, doc), "round trip");

    auto h = state::Record_header{};
    std::memcpy(&h, bytes.data(), sizeof h);
    expect(h.magic == state::record_magic && h.record_version == 1 && h.kind == 0, "header");
    expect(h.payload_length == 4 + 8 + 1 && bytes.size() == sizeof h + h.payload_length, "payload is what save wrote");

    auto padded = bytes;
    padded.resize(bytes.size() + 13, std::byte{0xee});
    auto back2 = Doc_v2{};
    expect(state::decode_record<Doc_v2>(padded, back2) && same(back2, doc), "trailing bytes after the record are ignored");
}

auto test_versions() -> void
{
    std::printf("author versions\n");

    auto old = Doc_v1{};
    old.notes = {9, 8, 7, 6, 5, 4, 3, 2};
    const auto v1_bytes = state::encode_record(old);

    auto doc = Doc_v2{};
    expect(state::decode_record<Doc_v2>(v1_bytes, doc), "v1 payload loads in v2");
    expect(doc.notes == old.notes && doc.swing == 50, "missing v2 field keeps its default");

    const auto v3_bytes = state::encode_record(Doc_v3{{1, 1, 1, 1, 1, 1, 1, 1}, 10, 120.f});
    auto from_future = Doc_v2{};
    from_future.swing = 77;
    expect(!state::decode_record<Doc_v2>(v3_bytes, from_future), "v3 payload refused by v2");
    expect(from_future.swing == 77, "refused load leaves the target untouched");
    expect(same(state::decode_record_or_default<Doc_v2>(v3_bytes), Doc_v2{}), "or_default gives the default");
}

auto test_refusals() -> void
{
    std::printf("refusals\n");

    auto doc = Doc_v2{};
    doc.swing = 1;
    const auto bytes = state::encode_record(doc);

    auto out = Doc_v2{};
    out.swing = 99;

    expect(!state::decode_record<Doc_v2>({}, out), "empty: nothing there");

    auto all_cut = true;
    for (auto n = std::size_t{1}; n < bytes.size(); ++n) {
        all_cut = all_cut && !state::decode_record<Doc_v2>(std::span{bytes}.first(n), out);
    }
    expect(all_cut && out.swing == 99, "every truncation refused");

    auto bad = bytes;
    bad[0] = std::byte{'x'};
    expect(!state::decode_record<Doc_v2>(bad, out), "wrong magic");

    bad = bytes;
    bad[4] = std::byte{2};
    expect(!state::decode_record<Doc_v2>(bad, out), "future container version");

    bad = bytes;
    bad[8] = std::byte{9};
    expect(!state::decode_record<Doc_v2>(bad, out), "unknown kind");

    // A payload_length shorter than what load needs: the reader stops at the record's end.
    bad = bytes;
    bad[12] = std::byte{6};
    expect(!state::decode_record<Doc_v2>(bad, out), "short payload_length bounds the reader");

    const auto refused = make_record(0, {42});
    auto r = Refusing_doc{};
    r.x = 5;
    expect(!state::decode_record<Refusing_doc>(refused, r) && r.x == 5, "load returning false keeps the target");

    const auto half = make_record(0, {500, 6});
    auto hd = Half_doc{};
    expect(!state::decode_record<Half_doc>(half, hd) && hd.x == 1 && hd.y == 2, "a half-done load does not leak");

    expect(same(out, [] { auto d = Doc_v2{}; d.swing = 99; return d; }()), "target untouched through all of it");
}

auto test_reader() -> void
{
    std::printf("reader\n");

    const auto bytes = std::array<std::byte, 6>{};
    auto payload = state::Payload_in{bytes, false};
    const auto in = payload.reader();
    const auto copy = in;
    auto a = std::uint32_t{};
    auto b = std::uint32_t{};
    auto c = std::uint8_t{};
    expect(in.read(a) && payload.remaining() == 2, "reads advance");
    expect(!copy.read(b) && payload.failed(), "a copied handle shares the cursor; overread fails");
    expect(!in.read(c), "a failed reader stays failed");
}

auto test_raw() -> void
{
    std::printf("raw fallback\n");

    const auto doc = Raw_doc{11, 2.5f};
    const auto bytes = state::encode_record(doc);

    auto h = state::Record_header{};
    std::memcpy(&h, bytes.data(), sizeof h);
    expect(h.kind == 1 && h.payload_length == sizeof(Raw_doc), "kind 1, the bytes of T");

    auto back = Raw_doc{};
    expect(state::decode_record<Raw_doc>(bytes, back) && same(back, doc), "round trip");

    auto bad = bytes;
    bad[12] = std::byte{4};
    auto keep = Raw_doc{};
    expect(!state::decode_record<Raw_doc>(bad, keep), "length other than sizeof(T) refused");

    const auto functions = state::encode_record(Doc_v1{});
    expect(!state::decode_record<Raw_doc>(functions, keep), "a functions record needs load()");
}

auto test_upgrade() -> void
{
    std::printf("raw -> functions upgrade\n");

    const auto old_bytes = state::encode_record(Raw_doc{21, 4.f});

    auto doc = Upgraded_doc{};
    doc.c = 77;
    expect(state::decode_record<Upgraded_doc>(old_bytes, doc), "raw record read by load()");
    expect(doc.a == 21 && doc.b == 4.f && doc.c == 3, "old fields carried, new field defaulted");

    const auto new_bytes = state::encode_record(Upgraded_doc{5, 1.f, 9});
    auto again = Upgraded_doc{};
    expect(state::decode_record<Upgraded_doc>(new_bytes, again) && again.c == 9, "then round-trips through functions");
}

// A stream that hands out `bytes`, the way CLAP's and VST3's readers do.
auto stream_of(std::span<const std::byte> bytes)
{
    return [bytes, at = std::size_t{}](std::byte* out, std::size_t n) mutable {
        if (bytes.size() - at < n) return false;
        std::memcpy(out, bytes.data() + at, n);
        at += n;
        return true;
    };
}

auto test_stream() -> void
{
    std::printf("stream reader\n");

    const auto record = state::encode_record(Doc_v2{});
    auto padded = record;
    padded.resize(record.size() + 9, std::byte{0x5a});
    expect(state::read_record(stream_of(padded)) == record, "reads exactly one record");
    expect(state::read_record(stream_of({})).empty(), "nothing after the chunk: empty");
    expect(state::read_record(stream_of(std::span{record}.first(record.size() - 1))).empty(), "a truncated record: empty");

    auto junk = std::vector<std::byte>(64, std::byte{0x41});
    expect(state::read_record(stream_of(junk)).empty(), "junk after the chunk: empty");

    // A header claiming 4 GB behind a short stream reads only what arrives.
    auto huge = make_record(0, {});
    huge[12] = huge[13] = huge[14] = huge[15] = std::byte{0xff};
    expect(state::read_record(stream_of(huge)).empty(), "a lying length stops at the stream's end");
}

auto test_base64() -> void
{
    std::printf("base64\n");

    expect(tiny::base64::encode(std::as_bytes(std::span{std::string_view{"Man"}})) == "TWFu", "known vector");
    expect(tiny::base64::encode(std::as_bytes(std::span{std::string_view{"Ma"}})) == "TWE=", "one pad");
    expect(tiny::base64::encode(std::as_bytes(std::span{std::string_view{"M"}})) == "TQ==", "two pads");

    auto all = true;
    for (auto n = std::size_t{}; n < 64; ++n) {
        auto bytes = std::vector<std::byte>(n);
        for (auto i = std::size_t{}; i < n; ++i) bytes[i] = static_cast<std::byte>(i * 37 + n);
        const auto text = tiny::base64::encode(bytes);
        const auto back = tiny::base64::decode(text);
        all = all && back && *back == bytes && text.find('\0') == std::string::npos;
    }
    expect(all, "round trip 0..63 bytes, no NULs");

    expect(!tiny::base64::decode("TWF"), "unpadded refused");
    expect(!tiny::base64::decode("TW=u"), "pad mid-quad refused");
    expect(!tiny::base64::decode("TQ==TWFu"), "pad before the end refused");
    expect(!tiny::base64::decode("TW\nu"), "whitespace refused");

    const auto record = state::encode_record(Doc_v2{});
    const auto back = tiny::base64::decode(tiny::base64::encode(record));
    auto doc = Doc_v2{};
    expect(back && state::decode_record<Doc_v2>(*back, doc), "record through base64");
}

} // namespace

auto main() -> int
{
    test_functions();
    test_versions();
    test_refusals();
    test_reader();
    test_raw();
    test_upgrade();
    test_stream();
    test_base64();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
