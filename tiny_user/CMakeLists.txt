# Your plug-in information goes here.
# ---
set(TINY_COMPANY_NAME "Sketch Audio")
set(TINY_COMPANY_WEBSITE "www.sketchaudio.com")
set(TINY_COMPANY_EMAIL "ryan@sketchaudio.com")

set(TINY_PRODUCT_NAME "Tiny Demo")
set(TINY_PRODUCT_SHORT_NAME "TnDm") # Must be <= 16 chars, for Pro Tools.
set(TINY_BASE_FILENAME "TinyDemo")
set(TINY_BASE_IDENTIFIER "com.sketch-audio.TinyDemo")
set(TINY_VERSION_STRING "1.0.0")
set(TINY_BUILD_NUMBER "1")

set(TINY_PLUGIN_WANTS_SIDECHAIN "false")

set(TINY_AAX_MANUFACTURER_ID "Skch")
set(TINY_AAX_PRODUCT_ID "demo")
set(TINY_AAX_PLUGIN_ID "demo")

# See: https://github.com/free-audio/clap/blob/main/include/clap/plugin-features.h
set(TINY_CLAP_DESCRIPTION "The tinyplug demo plug-in.")
set(TINY_CLAP_FEATURES "audio-effect" "stereo")

set(TINY_AUV2_VIEW_CLASS "TinyDemo_viewFactory")
set(TINY_AUV2_MANUFACTURER "Skch")
set(TINY_AUV2_TYPE "aufx")
set(TINY_AUV2_SUBTYPE "tndm")
set(TINY_AUV2_BUNDLE_VERSION "0x10600")
set(TINY_AUV2_BUNDLE_TAG "Effect") # Instrument or Effect

# See: https://steinbergmedia.github.io/vst3_doc/vstinterfaces/namespaceSteinberg_1_1Vst_1_1PlugType.html
set(TINY_VST3_CONTROLLER_UID "tiny" "Skch" "demo" "ctrl")
set(TINY_VST3_PROCESSOR_UID "tiny" "Skch" "demo" "proc")
set(TINY_VST3_SUBCATEGORIES "Fx")
# ---

# CMake helper functions
include(cmake/helpers.cmake)

# Prepare values for generated header.
# ---
prepare_clap_feature_list(
    TINY_CLAP_FEATURES 
    TINY_CLAP_FEATURE_COUNT TINY_CLAP_FEATURE_VALUES
)
prepare_vst3_uid_array(
    TINY_VST3_CONTROLLER_UID 
    TINY_VST3_CONTROLLER_UID_LITERALS
)
prepare_vst3_uid_array(
    TINY_VST3_PROCESSOR_UID 
    TINY_VST3_PROCESSOR_UID_LITERALS
)
# ---

# Configure generated header.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/plug_info.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/plug_info.h
)

set(TINY_USER_CODE ${TINY_BASE_FILENAME}_user)

file(GLOB USER_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.h")
file(GLOB USER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp")

add_library(${TINY_USER_CODE} STATIC
    ${USER_HEADERS}
    ${USER_SOURCES}
)

target_include_directories(${TINY_USER_CODE} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_BINARY_DIR} # for plug_info.h
)

target_link_libraries(${TINY_USER_CODE} PUBLIC ${TINY_SHARED_LIB})

# Export shared names to parent scope.
#---
macro(export_to_parent var)
    set(${var} ${${var}} PARENT_SCOPE)
endmacro()

export_to_parent(TINY_USER_CODE)

export_to_parent(TINY_COMPANY_NAME)
export_to_parent(TINY_COMPANY_WEBSITE)
export_to_parent(TINY_COMPANY_EMAIL)

export_to_parent(TINY_PRODUCT_NAME)
export_to_parent(TINY_PRODUCT_SHORT_NAME)
export_to_parent(TINY_BASE_FILENAME)
export_to_parent(TINY_BASE_IDENTIFIER)
export_to_parent(TINY_VERSION_STRING)
export_to_parent(TINY_BUILD_NUMBER)

export_to_parent(TINY_PLUGIN_WANTS_SIDECHAIN)

export_to_parent(TINY_AAX_MANUFACTURER_ID)
export_to_parent(TINY_AAX_PRODUCT_ID)
export_to_parent(TINY_AAX_PLUGIN_ID)

export_to_parent(TINY_CLAP_DESCRIPTION)
export_to_parent(TINY_CLAP_FEATURES)

export_to_parent(TINY_AUV2_VIEW_CLASS)
export_to_parent(TINY_AUV2_MANUFACTURER)
export_to_parent(TINY_AUV2_TYPE)
export_to_parent(TINY_AUV2_SUBTYPE)
export_to_parent(TINY_AUV2_BUNDLE_VERSION)
export_to_parent(TINY_AUV2_BUNDLE_TAG)

export_to_parent(TINY_VST3_CONTROLLER_UID)
export_to_parent(TINY_VST3_PROCESSOR_UID)
export_to_parent(TINY_VST3_SUBCATEGORIES)
#---