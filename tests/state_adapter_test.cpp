// Preset JSON and the state record: `State_adapter` carries it as base64 under "state".
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
    const auto json = a.preset_state();
    expect(json.contains("state") && json["state"].is_string(), "the record is a top-level base64 string");

    auto back = Doc{};
    expect(state::decode_record(a.state_record(json), back) && back.swing == 77, "and reads back");

    const auto none = adapter({}).preset_state();
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

// A preset saved by an older build (fixtures/presets/legacy.json, written at 9974496 by this test's
// since-removed writer): parameters by keypath, an author key and the framework's size keys under
// "editor", the document as base64.
const auto legacy_path = std::string{TINY_FIXTURES_DIR} + "/presets/legacy.json";

auto test_preset_and_session() -> void
{
    std::printf("presets and sessions\n");

    const auto edit = tiny::Edit_record{.size = tiny::Rect_size{613, 411}, .undoable = {std::byte{7}}, .restored = {}};
    auto with_edit = tiny::State_adapter{{
        .load_model = [] { return tiny::State_adapter::Load_model{.param_tree = &tree(), .num_params = 1}; },
        .save_model = [&] {
            auto legacy = tiny::State_map{};
            tiny::framework_keys::write(legacy, tiny::Framework_keys{.size = std::pair{613, 411}});
            return tiny::State_adapter::Save_model{.version = 1, .param_tree = &tree(), .param_values = {0.5},
                                                   .edit_record = tiny::encode_edit_record(edit), .legacy_editor = legacy};
        },
    }};

    const auto preset = with_edit.preset_state(tiny::Preset_info{.name = "Warm", .id = "factory/warm"});
    expect(preset.value("name", "") == "Warm" && preset.value("id", "") == "factory/warm", "a preset names itself");
    expect(!preset.contains("edit") && !preset.contains("editor"), "and carries no editor state");
    const auto info = with_edit.preset_info(preset);
    expect(info.name == "Warm" && info.id == "factory/warm", "its name and id read back");

    const auto session = with_edit.session_state();
    const auto back = with_edit.edit_record(session);
    expect(back && *back == edit, "a session carries the editor's record");
    const auto legacy = tiny::editor_size_state::extract(with_edit.editor_state(session));
    expect(legacy && legacy->first == 613, "and the size keys older builds read");
    expect(!with_edit.edit_record(preset), "a preset has none");
}

auto test_legacy_fixture() -> void
{
    std::printf("a preset saved by an older build\n");
    auto file = std::ifstream{legacy_path};
    expect(static_cast<bool>(file), "the fixture exists");
    if (!file) return;
    auto json = nlohmann::ordered_json{};
    file >> json;
    const auto a = adapter({});

    const auto values = a.param_values(json);
    expect(values.size() == 1 && values[0] && std::abs(*values[0] - 0.5) < 1e-6, "parameters read by keypath");

    const auto editor = a.editor_state(json);
    const auto* name = editor.contains("preset-name") ? std::get_if<std::string>(&editor.at("preset-name")) : nullptr;
    expect(name && *name == "Fixture preset", "the author's editor key reads back");
    const auto size = tiny::editor_size_state::extract(editor);
    expect(size && size->first == 613 && size->second == 411, "the framework's size keys read back");

    auto doc = Doc{};
    expect(state::decode_record(a.state_record(json), doc) && doc.swing == 77, "the document decodes");
}

} // namespace

auto main() -> int
{
    test_round_trip();
    std::printf("\n"); test_hostile();
    std::printf("\n"); test_reserved_keys();
    std::printf("\n"); test_preset_and_session();
    std::printf("\n"); test_legacy_fixture();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
