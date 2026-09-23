// Notes: the MIDI 1.0 codec, note identity, and the processor's outbox.
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <vector>

#include <tiny_core/midi_codec.hpp>
#include <tiny_core/midi_mpe.hpp>
#include <tiny_core/note_ids.hpp>
#include <tiny_core/note_out.hpp>

namespace {

using namespace tiny::process;
namespace midi = tiny::midi;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

template<typename T, typename V>
inline constexpr auto holds = false;
template<typename T, typename... Ts>
inline constexpr auto holds<T, std::variant<Ts...>> = (std::is_same_v<T, Ts> || ...);

template<typename T>
auto as(const midi::Decoded& d) -> const T*
{
    if constexpr (holds<T, Note::Any>) {
        if (const auto* n = std::get_if<Note::Any>(&d)) return std::get_if<T>(n);
    }
    else {
        if (const auto* c = std::get_if<Control::Any>(&d)) return std::get_if<T>(c);
    }
    return nullptr;
}

// MARK: - codec

auto test_codec() -> void
{
    std::printf("codec\n");

    const auto on = midi::decode(0x93, 60, 127);
    const auto* e_on = as<Note::On>(on);
    expect(e_on && e_on->note.channel == 3 && e_on->note.key == 60 && e_on->velocity == 1.f, "note on, channel 4");

    expect(as<Note::Off>(midi::decode(0x90, 60, 0)) != nullptr, "velocity 0 is an off");
    expect(as<Note::Off>(midi::decode(0x80, 60, 64)) != nullptr, "note off");

    const auto* poly = as<Note::Expression>(midi::decode(0xa0, 61, 127));
    expect(poly && poly->kind == Note::Expression::Kind::Pressure && poly->value == 1., "poly pressure is a per-note expression");

    const auto* pressure = as<Control::Pressure>(midi::decode(0xd2, 127, 0));
    expect(pressure && pressure->channel == 2 && pressure->value == 1., "channel pressure");

    const auto bend = [](uint8_t lsb, uint8_t msb) { return as<Control::Bend>(midi::decode(0xe0, lsb, msb))->value; };
    expect(bend(0, 0) == -1. && bend(0, 64) == 0. && bend(127, 127) == 1., "bend: both ends and the centre are exact");

    const auto* sustain = as<Control::Pedal>(midi::decode(0xb0, 64, 127));
    expect(sustain && sustain->kind == Control::Pedal::Kind::Sustain && sustain->value == 1., "CC 64 is the sustain pedal");
    expect(as<Control::Pedal>(midi::decode(0xb0, 1, 0))->kind == Control::Pedal::Kind::Mod_wheel, "CC 1 is the mod wheel");
    expect(std::holds_alternative<std::monostate>(midi::decode(0xb0, 20, 5)), "other controllers are dropped");
    expect(std::holds_alternative<midi::All_off>(midi::decode(0xb5, 123, 0)), "CC 123 is all notes off");
    expect(std::holds_alternative<std::monostate>(midi::decode(0xf8, 0, 0)), "system messages are dropped");

    // Round trips.
    auto ok = true;
    for (auto v = 0; v < 128; ++v) {
        const auto bytes = midi::encode(Note::Any{Note::Off{{0, 5, 70}, static_cast<float>(v) / 127.f}});
        ok = ok && bytes.size == 3 && bytes.data[0] == 0x85 && bytes.data[1] == 70 && bytes.data[2] == v;
    }
    expect(ok, "note off velocity round trips all 128 values");

    auto bend_ok = true;
    for (auto raw = 0; raw < 16384; raw += 17) {
        const auto d = midi::decode(0xe0, static_cast<uint8_t>(raw & 0x7f), static_cast<uint8_t>(raw >> 7));
        const auto bytes = midi::encode(Control::Any{*as<Control::Bend>(d)});
        bend_ok = bend_ok && (bytes.data[1] | (bytes.data[2] << 7)) == raw;
    }
    expect(bend_ok, "bend round trips");

    expect(midi::encode(Note::Any{Note::On{{0, 0, 60}, 0.f}}).data[2] == 1, "an on never encodes velocity 0");
    expect(midi::encode(Note::Any{Note::Expression{{}, Note::Expression::Kind::Tuning, 1.}}).size == 0, "per-note tuning has no MIDI 1.0 form");
    const auto pedal = midi::encode(Control::Any{Control::Pedal{0, Control::Pedal::Kind::Sustain, 1.}});
    expect(pedal.data[0] == 0xb0 && pedal.data[1] == 64 && pedal.data[2] == 127, "sustain encodes as CC 64");

    // Raw output: sent as written, channel voice messages only.
    const auto cc = midi::encode(midi::Raw::cc(2, 74, 90));
    expect(cc.size == 3 && cc.data[0] == 0xb2 && cc.data[1] == 74 && cc.data[2] == 90, "raw CC goes out as written");
    const auto program = midi::encode(midi::Raw::program(0, 5));
    expect(program.size == 2 && program.data[0] == 0xc0 && program.data[1] == 5, "program change is two bytes");
    expect(midi::encode(midi::Raw{{0xf8, 0, 0}}).size == 0, "system messages are refused");
    expect(midi::encode(midi::Raw{{0xb0, 200, 0}}).size == 0, "a data byte over 127 is refused");
    expect(midi::encode(midi::Raw{{0x40, 1, 2}}).size == 0, "a data byte in the status slot is refused");
}

// MARK: - identity

template<typename T>
auto named(Note_ids& ids, Note_ids::Source source, int32_t local, T event) -> std::pair<bool, uint32_t>
{
    auto any = Note::Any{event};
    const auto ok = ids.name(source, local, any);
    return {ok, std::visit([](const auto& e) { return e.note.id; }, any)};
}

auto test_ids() -> void
{
    std::printf("note identity\n");
    using S = Note_ids::Source;
    auto ids = Note_ids{};

    const auto a = named(ids, S::Host, -1, Note::On{{0, 0, 60}, 1.f});
    const auto b = named(ids, S::Host, -1, Note::On{{0, 0, 60}, 1.f});
    expect(a.first && b.first && a.second != b.second, "two ons for one key are two notes");

    const auto off1 = named(ids, S::Host, -1, Note::Off{{0, 0, 60}, 0.f});
    expect(off1.first && off1.second == a.second, "an id-less off closes the oldest");
    const auto expr = named(ids, S::Host, -1, Note::Expression{{0, 0, 60}, Note::Expression::Kind::Pressure, 0.5});
    expect(expr.first && expr.second == b.second, "and an expression reaches the one still held");
    named(ids, S::Host, -1, Note::Off{{0, 0, 60}, 0.f});
    expect(!named(ids, S::Host, -1, Note::Off{{0, 0, 60}, 0.f}).first, "an off for nothing held is dropped");

    // Host ids name notes exactly, even on one key.
    const auto h7 = named(ids, S::Host, 7, Note::On{{0, 0, 64}, 1.f});
    const auto h8 = named(ids, S::Host, 8, Note::On{{0, 0, 64}, 1.f});
    expect(named(ids, S::Host, 8, Note::Off{{0, 0, 64}, 0.f}).second == h8.second, "a host id closes its own note, not the oldest");
    expect(named(ids, S::Host, 99, Note::Off{{0, 0, 64}, 0.f}).second == h7.second, "an unknown host id falls back to channel + key");

    // Sources are separate.
    const auto host = named(ids, S::Host, -1, Note::On{{0, 0, 67}, 1.f});
    const auto edit = named(ids, S::Editor, -1, Note::On{{0, 0, 67}, 1.f});
    expect(named(ids, S::Editor, -1, Note::Off{{0, 0, 67}, 0.f}).second == edit.second, "an editor off never closes a host note");
    expect(named(ids, S::Host, -1, Note::Off{{0, 0, 67}, 0.f}).second == host.second, "and the host note is still there");

    // Two fingers on one key, each with the editor's own id.
    const auto f1 = named(ids, S::Editor, 1, Note::On{{0, 0, 72}, 1.f});
    const auto f2 = named(ids, S::Editor, 2, Note::On{{0, 0, 72}, 1.f});
    expect(named(ids, S::Editor, 2, Note::Expression{{0, 0, 72}, Note::Expression::Kind::Pressure, 1.}).second == f2.second,
           "editor ids tell two fingers on one key apart");
    expect(named(ids, S::Editor, 1, Note::Off{{0, 0, 72}, 0.f}).second == f1.second, "and close the right one");

    // Release all, by channel.
    ids.clear();
    named(ids, S::Host, -1, Note::On{{0, 0, 60}, 1.f});
    named(ids, S::Host, -1, Note::On{{0, 1, 60}, 1.f});
    named(ids, S::Editor, -1, Note::On{{0, 1, 62}, 1.f});
    auto released = 0;
    ids.release_all(S::Host, 1, [&](const Note::Off& off) { released += off.note.channel == 1 ? 1 : 100; });
    expect(released == 1, "release_all takes one channel of one source");

    // Full.
    ids.clear();
    auto all = true;
    for (auto i = size_t{}; i < Note_ids::capacity; ++i) all = all && named(ids, S::Host, static_cast<int32_t>(i), Note::On{{0, 0, 1}, 1.f}).first;
    expect(all && !named(ids, S::Host, -1, Note::On{{0, 0, 2}, 1.f}).first, "a full table drops the new note, not an old one");
}

// MARK: - outbox

auto test_outbox() -> void
{
    std::printf("outbox\n");
    auto box = Note_outbox{};
    const auto w = box.writer();

    box.begin_slice(0, 100);
    w.send(50, Note::Any{Note::On{{1, 0, 60}, 1.f}});
    w.send(10, Note::Any{Note::On{{2, 0, 62}, 1.f}});
    box.begin_slice(100, 28);
    w.send(0, Note::Any{Note::Off{{1, 0, 60}, 0.f}});
    w.send(500, Control::Any{Control::Bend{0, 0.5}});
    w.send(0, Note::Any{Note::Off{{2, 0, 62}, 0.f}});

    w.send(3, midi::Raw::cc(0, 1, 64));

    const auto events = box.events();
    expect(events.size() == 6, "six sent");
    expect(std::holds_alternative<midi::Raw>(events[4].event) && events[4].frame == 103, "raw messages share the outbox and its timing");
    expect(events[0].frame == 10 && events[1].frame == 50 && events[2].frame == 100, "slice frames become block frames, sorted");
    expect(std::holds_alternative<Note::Any>(events[2].event) && std::get<Note::Off>(std::get<Note::Any>(events[2].event)).note.id == 1
           && std::get<Note::Off>(std::get<Note::Any>(events[3].event)).note.id == 2, "ties keep send order");
    expect(events[5].frame == 127, "a frame past the slice clamps to its last frame");

    box.clear();
    box.begin_slice(0, 64);
    auto sent = size_t{};
    while (w.send(0, Control::Any{Control::Pressure{0, 1.}})) ++sent;
    expect(sent == Note_outbox::capacity, "sends refuse once full");
    expect(!Note_outbox::Writer{}.send(0, Control::Any{Control::Pressure{0, 1.}}), "an unbound writer refuses");
}


// MARK: - mpe

auto test_mpe() -> void
{
    std::printf("mpe\n");
    using Kind = Note::Expression::Kind;

    auto mpe = midi::Mpe{};
    const auto read = [&](uint8_t status, uint8_t d1, uint8_t d2) { mpe.observe(status, d1, d2); return mpe.expression(status, d1); };

    expect(!mpe.is_member(0) && mpe.is_member(1) && mpe.is_member(15), "no configuration: a lower zone of 15 members");

    const auto bend = read(0xe3, 127, 127); // Full up on channel 4.
    expect(bend && bend->kind == Kind::Tuning && bend->channel == 3 && std::abs(bend->value - 48.) < 0.01, "member bend is tuning, ±48 by default");
    expect(!read(0xe0, 0, 127), "manager bend stays a control");

    const auto press = read(0xd3, 127, 0);
    expect(press && press->kind == Kind::Pressure && press->value == 1., "member channel pressure is per-note pressure");
    const auto timbre = read(0xb3, 74, 0);
    expect(timbre && timbre->kind == Kind::Brightness && timbre->value == 0., "member CC 74 is brightness");
    expect(!read(0xb3, 64, 127), "other member controllers aren't expressions");

    auto inherited = std::vector<Kind>{};
    mpe.initial(3, [&](Kind kind, double) { inherited.push_back(kind); });
    expect(inherited.size() == 3, "a new note inherits what differs from neutral");
    read(0xe3, 0, 64); read(0xd3, 0, 0); read(0xb3, 74, 64);
    inherited.clear();
    mpe.initial(3, [&](Kind kind, double) { inherited.push_back(kind); });
    expect(inherited.size() == 1 && inherited[0] == Kind::Brightness, "neutral values aren't sent (CC 74 at 64 is just above 0.5)");

    // Bend sensitivity on a member sets the zone's: RPN 0, 12 semitones.
    read(0xb5, 101, 0); read(0xb5, 100, 0); read(0xb5, 6, 12);
    const auto narrow = read(0xe2, 127, 127);
    expect(narrow && std::abs(narrow->value - 12.) < 0.01, "RPN 0 on a member sets the zone's bend range");

    // Configuration: a lower zone of 7 on channel 1, then an upper zone of 3 on channel 16.
    read(0xb0, 101, 0); read(0xb0, 100, 6); read(0xb0, 6, 7);
    expect(mpe.is_member(7) && !mpe.is_member(8), "configuration sets the lower zone");
    const auto reset_range = read(0xe2, 127, 127);
    expect(reset_range && std::abs(reset_range->value - 48.) < 0.01, "configuration restores the default range");
    read(0xbf, 101, 0); read(0xbf, 100, 6); read(0xbf, 6, 3);
    expect(mpe.is_member(12) && mpe.is_member(14) && !mpe.is_member(11) && !mpe.is_member(15), "and the upper zone");
    read(0xbf, 6, 14);
    const auto lower_bend = read(0xe1, 127, 127);
    expect(mpe.is_member(1) && lower_bend && lower_bend->channel == 1, "an upper zone of 14 takes channel 2 from the lower zone");
    read(0xbf, 6, 0);
    expect(!mpe.is_member(14) && !mpe.is_member(1), "zero members: no zone");

    // An NRPN deselects, so its data entry changes nothing.
    auto fresh = midi::Mpe{};
    fresh.observe(0xb0, 101, 0); fresh.observe(0xb0, 100, 6); fresh.observe(0xb0, 99, 1); fresh.observe(0xb0, 6, 3);
    expect(fresh.is_member(15), "data entry after an NRPN is ignored");
}

} // namespace

auto main() -> int
{
    test_codec();
    std::printf("\n"); test_ids();
    std::printf("\n"); test_outbox();
    std::printf("\n"); test_mpe();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
