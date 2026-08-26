cmake_minimum_required(VERSION 3.25)

option(TINY_BUILD_PLUGINS "Build the test plug-ins." ON)
option(TINY_INSTALL_PLUGINS "Copy plug-ins to system folders after build (may prompt for admin privileges)" ON)

project(tinyplug VERSION 0.1.0) # Version code is used to generate names for Obj-C classes.

# Logging / lifecycle probing. On for Debug, off otherwise — the macros and every
# `log::Probe` method compile away entirely when it is off.
if(NOT DEFINED TINY_LOG)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR "Debug" IN_LIST CMAKE_CONFIGURATION_TYPES)
        set(TINY_LOG ON CACHE BOOL "Build the tiny::log logging layer.")
    else()
        set(TINY_LOG OFF CACHE BOOL "Build the tiny::log logging layer.")
    endif()
endif()
message(STATUS "[tiny] logging layer: ${TINY_LOG}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64") # Universal build.

    # Current requirement by Skia. We could possibly lower this but would require some investigation.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
    set(CMAKE_IPHONEOS_DEPLOYMENT_TARGET "14.0")

    # Xcode still complaining about std::filesystem::path in AUv3 build...
    set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "11.0")
    set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0")

    # If you want to run AUv3 on device.
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer")
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "") # You will need to set your team ID.
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

# tiny_deps path is required.
set(TINY_DEPS_PATH "" CACHE PATH "Path to tiny_deps.")
if (NOT TINY_DEPS_PATH)
    message(FATAL_ERROR "TINY_DEPS_PATH must be set.")
endif()
message(STATUS "[tiny] tiny_deps path: ${TINY_DEPS_PATH}")

include(FetchContent)
FetchContent_Declare(
    tiny_deps
    SOURCE_DIR ${TINY_DEPS_PATH}
)
FetchContent_MakeAvailable(tiny_deps)

include(cmake/helpers.cmake)
include(wrappers/wrappers.cmake)
include(tools/presets/make_preset_exporters.cmake)
include(tools/pagetables/make_pagetable.cmake)

# Core library.
add_subdirectory(libs/tinyplug)

# Optional, self-contained libraries (each its own CMakeLists, own include root).
# tiny_dsp is a pure leaf; tiny_platform depends on core, so it follows core.
add_subdirectory(libs/tiny_dsp)
add_subdirectory(libs/tiny_platform)

# Build the plug-ins.
if(TINY_BUILD_PLUGINS)
    add_subdirectory(examples)
endif()

# Forward SDK variables to parent scope only if this is NOT the top-level project.
if(NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)
    set(TINY_PLATFORM_LIB ${TINY_PLATFORM_LIB} PARENT_SCOPE)
    set(TINY_DSP_LIB ${TINY_DSP_LIB} PARENT_SCOPE)
endif()