set(PLUGIN_TARGET MeterDemo_lib) # Generated.

add_library(${PLUGIN_TARGET} STATIC)

# Your plug-in information goes here.
# ---
add_property(${PLUGIN_TARGET} TINY_COMPANY_NAME "tinyplug")
add_property(${PLUGIN_TARGET} TINY_COMPANY_WEBSITE "https://github.com/sketch-audio/tinyplug")
add_property(${PLUGIN_TARGET} TINY_COMPANY_EMAIL "info@tinyplug.com")

# These need to be four-character codes.
add_property(${PLUGIN_TARGET} TINY_FRAMEWORK_CODE "tiny")
add_property(${PLUGIN_TARGET} TINY_MANUFACTURER_CODE "Tiny") # At least one capital letter.
add_property(${PLUGIN_TARGET} TINY_PLUGIN_CODE "mtrd") # Must be unique per manufacturer code!

add_property(${PLUGIN_TARGET} TINY_PRODUCT_NAME "Meter Demo")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_NAME "Meter Demo")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_SHORT_NAME "MeterDemo") # Must be <= 16 chars, for Pro Tools.

add_property(${PLUGIN_TARGET} TINY_BASE_FILENAME "MeterDemo")
add_property(${PLUGIN_TARGET} TINY_BASE_IDENTIFIER "com.tinyplug.MeterDemo")
add_property(${PLUGIN_TARGET} TINY_VERSION_STRING "1.0.0")
add_property(${PLUGIN_TARGET} TINY_BUILD_NUMBER "1")

add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_SIDECHAIN "false")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_CAN_PROCESS_MONO "false")

# AAX
# See: AAX_Enums.h for possible values.
add_property(${PLUGIN_TARGET} TINY_AAX_CATEGORIES "AAX_ePlugInCategory_None")

# CLAP
# See: https://github.com/free-audio/clap/blob/main/include/clap/plugin-features.h
add_property(${PLUGIN_TARGET} TINY_CLAP_DESCRIPTION "Built with tinyplug.")
add_property(${PLUGIN_TARGET} TINY_CLAP_FEATURES "audio-effect") # Don't put mono or stereo, the framework will do that for you!

# AUv2
add_property(${PLUGIN_TARGET} TINY_AUV2_VIEW_CLASS "MeterDemo_viewFactory") # Audio Unit must have a unique view factory!
add_property(${PLUGIN_TARGET} TINY_AUV2_TYPE "aufx")

# VST3
# See: https://steinbergmedia.github.io/vst3_doc/vstinterfaces/namespaceSteinberg_1_1Vst_1_1PlugType.html
add_property(${PLUGIN_TARGET} TINY_VST3_SUBCATEGORIES "Fx")
# ---

# Your plug-in's sources
# ---
target_sources(${PLUGIN_TARGET} PRIVATE
    source/models/meters.hpp
    source/models/params.hpp
    source/editor.cpp
    source/editor.hpp
    source/processor.cpp
    source/processor.hpp
)
# ---

# Generate your plug-in's `plug_info.h`
# ---
configure_plug_info(
    ${PLUGIN_TARGET}
    ${CMAKE_CURRENT_BINARY_DIR}/plug_info.hpp
)
# ---

target_include_directories(${PLUGIN_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_BINARY_DIR} # for plug_info.hpp
)
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_SHARED_LIB}) # plugin headers expose core types
target_link_libraries(${PLUGIN_TARGET} PRIVATE ${TINY_PLATFORM_LIB}) # editor may use platform services (dialogs/paths)
target_link_libraries(${PLUGIN_TARGET} PRIVATE tiny::skia) # editor draws with Skia

# Build the wrappers
# ---
make_aax_plugin(${PLUGIN_TARGET})
make_auv2_plugin(${PLUGIN_TARGET}) # Skips build if not Apple.
make_auv3_plugin(${PLUGIN_TARGET})
make_clap_plugin(${PLUGIN_TARGET})
make_vst3_plugin(${PLUGIN_TARGET})
# ---