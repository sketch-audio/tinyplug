// Preset JSON and the state record: `State_adapter` carries it as base64 under "state".
#include <cstdio>
#include <string>
#include <vector>

#include <tiny_core/base64.hpp>
#include <tiny_core/state_adapter.hpp>
#include <tiny_core/state_record.hpp>

namespace {

namespace state = tiny::state;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

struct Doc {
    std::uint8_t swing{50};

    static auto save(state::Writer out, const Doc& value) -> bool
    {
        return out.write(std::uint32_t{1}) && out.write(value.swing);
    }

    static auto load(state::Reader in, Doc& value) -> bool
    {
        auto version = std::uint32_t{};
        return in.read(version) && version <= 1 && in.read(value.swing);
    }
};

auto tree() -> const tiny::params::Node&
{
    using namespace tiny::params;
    static const auto node = Node{Group{.nodes = {
        Spec{.identity = {.address = 0, .identifier = "depth"}, .name = "Depth",
             .semantics = Semantics::Real{.min_val = 0, .def_val = 1, .max_val = 1}},
    }}};
    return node;
}

auto adapter(std::vector<std::byte> record) -> tiny::State_adapter
{
    return tiny::State_adapter{{
        .load_model = [] { return tiny::State_adapter::Load_model{.param_tree = &tree(), .num_params = 1}; },
        .save_model = [record] {
            return tiny::State_adapter::Save_model{
                .version = 1, .param_tree = &tree(), .param_values = {0.5}, .state_record = record};
        },
    }};
}

auto test_round_trip() -> void
{
    std::printf("preset json\n");

    const auto doc = Doc{77};
    const auto a = adapter(state::encode_record(doc));
    const auto json = a.preset_state({});
    expect(json.contains("state") && json["state"].is_string(), "the record is a top-level base64 string");

    auto back = Doc{};
    expect(state::decode_record(a.state_record(json), back) && back.swing == 77, "and reads back");

    const auto none = adapter({}).preset_state({});
    expect(!none.contains("state"), "no document, no key");
    expect(adapter({}).state_record(none).empty(), "a preset without one reads as empty");
}

auto test_hostile() -> void
{
    std::printf("hostile presets\n");
    const auto a = adapter({});

    auto json = nlohmann::ordered_json{};
    json["state"] = 42;
    expect(a.state_record(json).empty(), "not a string");

    json["state"] = "not base64!";
    expect(a.state_record(json).empty(), "not base64");

    json["state"] = "AAAA";
    const auto junk = a.state_record(json);
    auto doc = Doc{};
    expect(!junk.empty() && !state::decode_record(junk, doc) && doc.swing == 50, "base64 of junk decodes to a refused record");

    // A preset from a newer build: the author's load refuses the version.
    auto newer = std::vector<std::byte>{};
    auto payload = state::Payload_out{};
    payload.write(state::Record_header{state::record_magic, state::record_version, 0, 5});
    payload.write(std::uint32_t{9});
    payload.write(std::uint8_t{1});
    json["state"] = tiny::base64::encode(payload.bytes());
    expect(!state::decode_record(a.state_record(json), doc) && doc.swing == 50, "a future version keeps the default");
}

auto test_reserved_keys() -> void
{
    std::printf("reserved keys\n");
    auto map = tiny::State_map{{"colour", 3}, {"theme", std::string{"dark"}}};
    tiny::drop_reserved_keys(map);
    expect(map.size() == 2, "ordinary keys stay");
}

} // namespace

auto main() -> int
{
    test_round_trip();
    std::printf("\n"); test_hostile();
    std::printf("\n"); test_reserved_keys();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
