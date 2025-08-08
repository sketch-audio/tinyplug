cmake_minimum_required(VERSION 3.25)

option(TINY_INSTALL_PLUGINS "Copy plug-ins to system folders after build (may prompt for admin privileges)" ON)

project(tinyplug)

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

# Shared library & user code.
add_subdirectory(tiny_shared)
add_subdirectory(tiny_user)

# Plug-in targets.
add_subdirectory(tiny_aax)
add_subdirectory(tiny_auv2)
add_subdirectory(tiny_clap)
add_subdirectory(tiny_vst3)