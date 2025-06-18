#pragma once

#include "tinyplug.h"

namespace tiny {

struct User_gui {};
struct User_dsp {};

struct User_plug {
    //
    static inline const auto info = Plug_info{
        .company_name = "Sketch Audio",
        .company_website = "www.sketchaudio.com",
        .company_email = "ryan@sketchaudio.com",
        .aax_manufacturer_id = 'Skch',
        .aax_product_id = 'demo',
        .aax_plugin_id = 'demo',
        .clap_description = "My cool audio effect.",
        .clap_features = {"audio-effect", "stereo", nullptr},
        .vst3_controller_uid = {'tiny', 'Skch', 'demo', 'ctrl'},
        .vst3_processor_uid = {'tiny', 'Skch', 'demo', 'proc'},
        .vst3_subcategories = "Fx",
    };

    static constexpr auto io = Plug_io{
        .audio_ports = {.num_inputs = 1, .num_outputs = 1},
        .midi_ports = {.num_inputs = 0, .num_outputs = 0},
    };

    using Gui = User_gui;
    using Dsp = User_dsp;
};

} // namespace tiny
