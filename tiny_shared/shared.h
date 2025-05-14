#pragma once

#include "tinyplug.h"

namespace tiny {

struct User_plug {
    static constexpr auto info = Plug_info{
        .company_name = "Sketch Audio",
        .company_website = "www.sketchaudio.com",
        .company_email = "ryan@sketchaudio.com",
        .vst3_controller_uid = {'tiny', 'Skch', 'demo', 'ctrl'},
        .vst3_processor_uid = {'tiny', 'Skch', 'demo', 'proc'},
        .vst3_subcategories = "Fx",
    };

    static constexpr auto io = Plug_io{
        .audio_ports = {.num_inputs = 1, .num_outputs = 1},
        .midi_ports = {.num_inputs = 0, .num_outputs = 0},
    };
};

} // namespace tiny
