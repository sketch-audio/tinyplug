cmake_minimum_required(VERSION 3.25)

option(TINY_BUILD_PLUGINS "Build the test plug-ins." ON)
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
FetchContent_MakeAvailable(tiny_deps)

include(cmake/helpers.cmake)
include(formats/formats.cmake)
include(presets/make_preset_exporter.cmake)

# Add shared library.
add_subdirectory(shared)

# Build the plug-ins.
if(TINY_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

# Forward SDK variables to parent scope only if this is NOT the top-level project.
if(NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)
endif()