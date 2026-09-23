set(PLUGIN_TARGET StepSequencer_lib) # Generated.

add_library(${PLUGIN_TARGET} STATIC)

# Your plug-in information goes here.
# ---
add_property(${PLUGIN_TARGET} TINY_COMPANY_NAME "tinyplug")
add_property(${PLUGIN_TARGET} TINY_COMPANY_WEBSITE "https://github.com/sketch-audio/tinyplug")
add_property(${PLUGIN_TARGET} TINY_COMPANY_EMAIL "info@tinyplug.com")

# These need to be four-character codes.
add_property(${PLUGIN_TARGET} TINY_FRAMEWORK_CODE "tiny")
add_property(${PLUGIN_TARGET} TINY_MANUFACTURER_CODE "Tiny") # At least one capital letter.
add_property(${PLUGIN_TARGET} TINY_PLUGIN_CODE "stps") # Must be unique per manufacturer code!

add_property(${PLUGIN_TARGET} TINY_PRODUCT_NAME "Step Sequencer")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_NAME "Step Sequencer")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_SHORT_NAME "StepSequencer") # Must be <= 16 chars, for Pro Tools.

add_property(${PLUGIN_TARGET} TINY_BASE_FILENAME "StepSequencer")
add_property(${PLUGIN_TARGET} TINY_BASE_IDENTIFIER "com.tinyplug.StepSequencer")
add_property(${PLUGIN_TARGET} TINY_VERSION_STRING "1.0.0")
add_property(${PLUGIN_TARGET} TINY_BUILD_NUMBER "1")

# An instrument that emits notes, with a silent audio output: the one shape every host loads,
# Live included (route another track's "MIDI From" to it). Declaring `TINY_PLUGIN_WANTS_AUDIO
# "none"` instead makes it a pure note effect: Logic's MIDI FX slot (`aumi`), a Pro Tools MIDI
# effect, a CLAP note effect, but no Live, and a VST3 some hosts refuse.
add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_AUDIO "out")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_NOTES "in;out")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_CAN_PROCESS_MONO "false")

# AAX
# See: AAX_Enums.h for possible values.
add_property(${PLUGIN_TARGET} TINY_AAX_CATEGORIES "AAX_ePlugInCategory_None")

# CLAP
# See: https://github.com/free-audio/clap/blob/main/include/clap/plugin-features.h
add_property(${PLUGIN_TARGET} TINY_CLAP_DESCRIPTION "Built with tinyplug.")
add_property(${PLUGIN_TARGET} TINY_CLAP_FEATURES "utility") # Don't put mono or stereo, the framework will do that for you!

# AUv2
add_property(${PLUGIN_TARGET} TINY_AUV2_VIEW_CLASS "StepSequencer_viewFactory") # Audio Unit must have a unique view factory!

# VST3
# See: https://steinbergmedia.github.io/vst3_doc/vstinterfaces/namespaceSteinberg_1_1Vst_1_1PlugType.html
add_property(${PLUGIN_TARGET} TINY_VST3_SUBCATEGORIES "Instrument")
# ---

# Your plug-in's sources
# ---
target_sources(${PLUGIN_TARGET} PRIVATE
    source/models/meters.hpp
    source/models/params.hpp
    source/models/state.hpp
    source/editor.cpp
    source/editor.hpp
    source/processor.cpp
    source/processor.hpp
)
# ---

# Generate your plug-in's `plug_info.hpp`
# ---
configure_plug_info(
    ${PLUGIN_TARGET}
    ${CMAKE_CURRENT_BINARY_DIR}/plug_info.hpp
)
configure_models(${PLUGIN_TARGET}) # <tiny_models.hpp> from source/models/
configure_plugin(${PLUGIN_TARGET}) # <tiny_plugin.hpp> from source/
# ---

target_include_directories(${PLUGIN_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_BINARY_DIR} # for plug_info.hpp
)
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_PLUG_LIB}) # plugin headers expose framework types
target_link_libraries(${PLUGIN_TARGET} PRIVATE ${TINY_PLATFORM_LIB}) # editor may use platform services (dialogs/paths)
target_link_libraries(${PLUGIN_TARGET} PRIVATE tiny::skia) # editor draws with Skia

# Build the wrappers
# ---
make_aax_plugin(${PLUGIN_TARGET})  # An instrument whose MIDI output routes to MIDI tracks.
make_auv2_plugin(${PLUGIN_TARGET}) # `aumu`: Logic ignores its MIDI output; the note-effect build is the Logic one.
make_auv3_plugin(${PLUGIN_TARGET})
make_clap_plugin(${PLUGIN_TARGET})
make_vst3_plugin(${PLUGIN_TARGET})
# ---
