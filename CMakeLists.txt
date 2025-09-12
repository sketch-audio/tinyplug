cmake_minimum_required(VERSION 3.25)

option(TINY_INSTALL_PLUGINS "Copy plug-ins to system folders after build (may prompt for admin privileges)" ON)

project(tinyplug VERSION 0.1.0) # Version code is used to generate names for Obj-C classes.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64") # Universal build.

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
FetchContent_MakeAvailable(tiny_deps) # skia skia-window-contexts

# AAX SDK path is optional.
set(TINY_AAX_SDK_PATH "" CACHE PATH "Path to AAX SDK.")
if (NOT TINY_AAX_SDK_PATH)
    set(TINY_AAX_SDK_PATH "n/a") # !!!
endif()

# NEW BUILD
include(cmake/helpers.cmake)
include(formats/formats.cmake)

# Enable SDKs.
set(AAX_SDK_ROOT_DIR ${TINY_AAX_SDK_PATH})
message(STATUS "[tiny] AAX SDK Root Directory: ${AAX_SDK_ROOT_DIR}")

set(AUV2_SDK_VER 1.3.0) # See: https://github.com/apple/AudioUnitSDK/tags
enable_auv2_sdk(${AUV2_SDK_VER} AUV2_SDK)
message(STATUS "[tiny] AUv2 SDK Version: ${AUV2_SDK_VER}")
message(STATUS "[tiny] AUv2 SDK: ${AUV2_SDK}")

set(CLAP_SDK_VER 1.2.6) # See: https://github.com/free-audio/clap/tags
enable_clap_sdk(${CLAP_SDK_VER} CLAP_SDK CLAP_HELPERS)
message(STATUS "[tiny] CLAP SDK Version: ${CLAP_SDK_VER}")
message(STATUS "[tiny] CLAP SDK: ${CLAP_SDK}")
message(STATUS "[tiny] CLAP Helpers: ${CLAP_HELPERS}")

set(VST3_SDK_VER v3.7.14_build_55) # See: https://github.com/steinbergmedia/vst3sdk/tags
enable_vst3_sdk(${VST3_SDK_VER} VST3_SDK VST3_SDK_ROOT_DIR)
message(STATUS "[tiny] VST3 SDK Version: ${VST3_SDK_VER}")
message(STATUS "[tiny] VST3 SDK: ${VST3_SDK}")
message(STATUS "[tiny] VST3 SDK Root Directory: ${VST3_SDK_ROOT_DIR}")

# Add shared library.
add_subdirectory(shared)

# Build the plug-ins.
add_subdirectory(plugins)