#pragma once

#include "tinyplug.h"

namespace tiny {

struct User_gui {};
struct User_dsp {};

struct User_plug {
    //
    static constexpr auto io = Plug_io{
        .audio_ports = {.num_inputs = 1, .num_outputs = 1},
        .midi_ports = {.num_inputs = 0, .num_outputs = 0},
    };

    using Gui = User_gui;
    using Dsp = User_dsp;
};

} // namespace tiny
