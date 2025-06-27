cmake_minimum_required(VERSION 3.25) # ???

project(tinyplug)

# Your plug-in information goes here.
set(tiny_manufacturer_name "Sketch Audio")
set(tiny_product_name "Tiny Demo")
set(tiny_product_short_name "TnDm") # Must be <= 16 chars, for Pro Tools.
set(tiny_base_filename "TinyDemo")
set(tiny_base_identifier "com.sketch-audio.TinyDemo")
set(tiny_version_string "1.0.0")
set(tiny_build_number "1")

set(tiny_auv2_manufacturer "Skch")
set(tiny_auv2_type "aufx")
set(tiny_auv2_subtype "tndm")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64") # Universal build.
elseif(WIN32)
    # This is for compatibility with Skia.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    endif()
    add_compile_options("/utf-8") # for MSVC
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