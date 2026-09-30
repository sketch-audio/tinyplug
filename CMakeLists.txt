cmake_minimum_required(VERSION 3.25)

option(TINY_BUILD_PLUGINS "Build the test plug-ins." ON)
option(TINY_INSTALL_PLUGINS "Copy plug-ins to system folders after build (may prompt for admin privileges)" ON)
option(TINY_BUILD_TESTS "Build the tests under tests/." OFF)
set(TINY_SANITIZE "" CACHE STRING "Sanitizers for the whole configure, e.g. thread or address,undefined. Empty for none.")

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

# Sanitizers instrument everything built from source, deps included: a partly instrumented
# link is where TSan's false reports come from. One slice, since TSan builds one at a time.
if(TINY_SANITIZE AND MSVC)
    # MSVC has ASan only. UBSan is dropped with a note so the `address,undefined` spelling still
    # configures; TSan has no MSVC equivalent at all.
    if(TINY_SANITIZE MATCHES "thread")
        message(FATAL_ERROR "[tiny] MSVC has no ThreadSanitizer.")
    endif()
    if(NOT TINY_SANITIZE MATCHES "address")
        message(FATAL_ERROR "[tiny] MSVC supports TINY_SANITIZE=address only.")
    endif()
    if(TINY_SANITIZE MATCHES "undefined")
        message(STATUS "[tiny] MSVC has no UBSan; building with address only.")
    endif()
    # ASan rejects /RTC1 and incremental linking, which the Debug defaults turn on.
    foreach(_lang C CXX)
        string(REPLACE "/RTC1" "" CMAKE_${_lang}_FLAGS_DEBUG "${CMAKE_${_lang}_FLAGS_DEBUG}")
    endforeach()
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)
    # Otherwise the VST3 SDK adds /ZI (Edit and Continue), which ASan rejects.
    set(SMTG_ENABLE_ADDRESS_SANITIZER ON CACHE BOOL "" FORCE)
    # Prebuilt Skia and AAX aren't instrumented. With container annotations on, the STL stamps a
    # /failifmismatch the prebuilt objects can't match, and a vector touched on both sides of
    # that line would report false overflows. Off everywhere keeps the link consistent.
    add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    # The runtime is one DLL beside cl.exe, for /MT and /MD alike. tiny_copy_asan_runtime(<target>)
    # puts it next to an executable so it runs without a developer prompt's PATH.
    get_filename_component(_msvc_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(TINY_ASAN_RUNTIME ${_msvc_bin}/clang_rt.asan_dynamic-x86_64.dll)
    if(NOT EXISTS ${TINY_ASAN_RUNTIME})
        message(FATAL_ERROR "[tiny] ASan runtime not found at ${TINY_ASAN_RUNTIME}. Install the MSVC ASan component.")
    endif()
    message(STATUS "[tiny] sanitizers: address (MSVC)")
elseif(TINY_SANITIZE)
    if(APPLE)
        set(CMAKE_OSX_ARCHITECTURES ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()
    add_compile_options(-fsanitize=${TINY_SANITIZE} -fno-omit-frame-pointer -O1)
    add_link_options(-fsanitize=${TINY_SANITIZE})
    message(STATUS "[tiny] sanitizers: ${TINY_SANITIZE}")
endif()

function(tiny_copy_asan_runtime target)
    if(TINY_ASAN_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TINY_ASAN_RUNTIME} $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()

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

# Under a sanitizer, compile the AU SDK from the sources tiny_deps ships instead of linking its
# prebuilt archive, so the SDK's code is instrumented too.
if(TINY_SANITIZE AND TARGET ausdk)
    file(GLOB _ausdk_sources ${TINY_DEPS_PATH}/third_party/AudioUnitSDK/src/AudioUnitSDK/*.cpp)
    add_library(tiny_ausdk_from_source STATIC ${_ausdk_sources})
    target_include_directories(tiny_ausdk_from_source SYSTEM PUBLIC ${TINY_DEPS_PATH}/third_party/AudioUnitSDK/include)
    set_property(TARGET ausdk PROPERTY INTERFACE_LINK_LIBRARIES tiny_ausdk_from_source "-framework AudioToolbox -framework CoreFoundation")
endif()
if(TINY_SANITIZE AND APPLE AND TARGET aaxsdk) # Likewise the AAX library, whose chunk parser the data model shares across threads.
    set(_aax_root ${TINY_DEPS_PATH}/third_party/aax-sdk)
    file(GLOB _aax_sources ${_aax_root}/Libs/AAXLibrary/source/*.cpp)
    list(FILTER _aax_sources EXCLUDE REGEX "\\.Win\\.cpp$")
    add_library(tiny_aaxlib_from_source STATIC ${_aax_sources} ${_aax_root}/Libs/AAXLibrary/source/AAX_CAutoreleasePool.OSX.mm ${_aax_root}/Interfaces/ACF/CACFClassFactory.cpp)
    target_include_directories(tiny_aaxlib_from_source SYSTEM PRIVATE ${_aax_root}/Libs/AAXLibrary/include ${_aax_root}/Interfaces ${_aax_root}/Interfaces/ACF)
    target_compile_options(tiny_aaxlib_from_source PRIVATE -w)
    # Known SDK bug, to report to Avid: LoadChunk over-reads a truncated chunk. Left uninstrumented
    # for ASan only, so the hostile-chunk scenarios still check our side of SetChunk.
    if(TINY_SANITIZE MATCHES "address")
        set_source_files_properties(${_aax_root}/Libs/AAXLibrary/source/AAX_CChunkDataParser.cpp PROPERTIES COMPILE_OPTIONS -fno-sanitize=address)
    endif()
    set_property(TARGET aaxsdk PROPERTY INTERFACE_LINK_LIBRARIES tiny_aaxlib_from_source "-framework Foundation")
endif()

include(cmake/helpers.cmake)
include(wrappers/wrappers.cmake)
include(tools/presets/make_preset_exporters.cmake)
include(tools/pagetables/make_pagetable.cmake)

# Framework. tiny_core is model-free and compiled once; tinyplug is the header-only,
# model-aware half and only ever compiles inside a plug-in target.
add_subdirectory(libs/tiny_core)
add_subdirectory(libs/tinyplug)

# Optional, self-contained libraries (each its own CMakeLists, own include root).
# tiny_dsp is a pure leaf; tiny_platform depends on core, so it follows core; tiny_ui
# is header-only on top of tinyplug.
add_subdirectory(libs/tiny_dsp)
add_subdirectory(libs/tiny_platform)
add_subdirectory(libs/tiny_ui)

# Build the plug-ins.
if(TINY_BUILD_PLUGINS)
    add_subdirectory(examples)
endif()

if(TINY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Forward SDK variables to parent scope only if this is NOT the top-level project.
if(NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(TINY_CORE_LIB ${TINY_CORE_LIB} PARENT_SCOPE)
    set(TINY_PLUG_LIB ${TINY_PLUG_LIB} PARENT_SCOPE)
    set(TINY_PLATFORM_LIB ${TINY_PLATFORM_LIB} PARENT_SCOPE)
    set(TINY_DSP_LIB ${TINY_DSP_LIB} PARENT_SCOPE)
    set(TINY_UI_LIB ${TINY_UI_LIB} PARENT_SCOPE)
endif()