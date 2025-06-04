#pragma once

#include "clap/helpers/plugin.hh"
#include "cmake_defines.h"
#include "user_plug.h"

namespace tiny {

static const clap_plugin_descriptor_t descriptor{
    .clap_version = CLAP_VERSION,
    .id = Cmake_defines::base_identifier.c_str(),
    .name = Cmake_defines::product_name.c_str(),
    .vendor = User_plug::info.company_name.c_str(),
    .url = User_plug::info.company_website.c_str(),
    .manual_url = User_plug::info.company_website.c_str(),
    .support_url = User_plug::info.company_website.c_str(),
    .version = Cmake_defines::version_string.c_str(),
    .description = User_plug::info.clap_description.c_str(),
    .features = User_plug::info.clap_features.data()
};

}