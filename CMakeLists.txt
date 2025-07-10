cmake_minimum_required(VERSION 3.25)

option(TINY_INSTALL_PLUGINS "Copy plug-ins to system folders after build (may prompt for admin privileges)" ON)

project(tinyplug)

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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64") # Universal build.
elseif(WIN32)
    # Use the static CRT. (This is how we built skia.)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    endif()
endif()

# We need a path to the prebuilt Skia library.
set(TINY_SKIA_ROOT_DIR "" CACHE PATH "Path to Skia library.")
if(NOT TINY_SKIA_ROOT_DIR)
    message(FATAL_ERROR "TINY_SKIA_ROOT_DIR must be set.")
endif()
message(STATUS "TINY_SKIA_ROOT_DIR: ${TINY_SKIA_ROOT_DIR}")

# AAX SDK path is optional.
set(TINY_AAX_SDK_DIR "" CACHE PATH "Path to AAX SDK.")

# Shared library.
add_subdirectory(tiny_shared)

# Plug-in targets.
add_subdirectory(tiny_aax)
add_subdirectory(tiny_auv2)
add_subdirectory(tiny_clap)
add_subdirectory(tiny_vst3)