// The web processor module compiled natively, rendering compare.mjs's schedule: the same wrapper code as the wasm
// build, so only the compiler, the target and the maths library differ. usage: <exe> <out.f32>
// Includes module.cpp: its table holds 32-bit addresses, which a native pointer doesn't fit.
#include "../../wrappers/web/source/processor/module.cpp"

#include <cstdio>
#include <vector>

namespace {

using namespace tiny::web;

auto plain_at(double knob) -> double
{
    return tiny::params::Value_helper::knob_to_plain(knob, tiny::User_params::param_spec(0).semantics);
}

} // namespace

auto main(int argc, char** argv) -> int
{
    if (argc < 2) return 2;
    tiny_web_create();
    auto& m = *module_instance;
    tiny_web_configure(48000);

    auto out = std::vector<float>{};
    auto seed = std::uint32_t{1};
    for (auto block = 0; block < 120; ++block) {
        // compare.mjs pushes the same messages before the same blocks.
        if (block == 2 && tiny::Plug_info::Wants::notes_in) m.inbound.push_value(Kind::Midi, Midi{.status = 0x90, .data1 = 60, .data2 = 100, .offset = 11});
        if (block == 10) m.inbound.push_value(Kind::Param_set, Param_set{.value = plain_at(1), .address = 0, .offset = 37});
        if (block == 40) m.inbound.push_value(Kind::Param_ramp, Param_ramp{.value = plain_at(0), .address = 0, .samples = 3000, .offset = 5});
        if (block == 50 && tiny::Plug_info::Wants::notes_in) m.inbound.push_value(Kind::Midi, Midi{.status = 0x80, .data1 = 60, .data2 = 0, .offset = 3});
        if (block == 60) m.inbound.push_value(Kind::Bypass, Flag{.on = 1});
        if (block == 80) m.inbound.push_value(Kind::Bypass, Flag{.on = 0});
        for (auto i = 0; i < 128; ++i) {
            seed = seed * 1103515245u + 12345u;
            const auto x = static_cast<float>(static_cast<std::int32_t>(seed >> 8) - (1 << 23)) / static_cast<float>(1 << 24);
            for (auto& ch : m.inputs) ch[i] = x;
        }
        tiny_web_process(128);
        for (auto i = 0; i < 128; ++i) for (const auto& ch : m.outputs) out.push_back(ch[i]);
        m.outbound.drain([](Kind, const void*, std::uint32_t) {});
    }
    auto* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(out.data(), sizeof(float), out.size(), f);
    std::fclose(f);
    return 0;
}
