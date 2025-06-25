# Configure generated header.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake_defines.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_defines.h
)

# --- SKIA ---
# See example: https://github.com/olilarkin/skia-builder

# Platform-specific settings.
if(APPLE)
    set(CMAKE_MACOSX_RPATH 1)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
elseif(WIN32)
    # Windows settings at top level.
endif()

# Set include and library directories.
unset(SKIA_LIB CACHE)
if(APPLE)
    # TODO: - Update for new iPlug2 dependencies directory.
    set(SKIA_INCLUDE_DIR ${TINY_SKIA_ROOT_DIR}/build/include)
    set(SKIA_LIB_DIR ${TINY_SKIA_ROOT_DIR}/build/mac/lib/Release)
    find_library(SKIA_LIB skia PATHS ${SKIA_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
elseif(WIN32)
    set(SKIA_INCLUDE_DIR ${TINY_SKIA_ROOT_DIR}/src/skia)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(SKIA_LIB_DIR ${TINY_SKIA_ROOT_DIR}/win/x64/Debug)
    else()
        set(SKIA_LIB_DIR ${TINY_SKIA_ROOT_DIR}/win/x64/Release)
    endif()
    find_library(SKIA_LIB skia PATHS ${SKIA_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
endif()

# Enable Link Time Optimization if supported (and not using Emscripten).
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)
if(supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO is not supported: ${error}")
endif()
# --- SKIA ---

if(APPLE)
    set(PLATFORM_SOURCES
        platform/mac_view.mm
    )
elseif(WIN32)
    set(PLATFORM_SOURCES
        platform/win_view.cpp
    )
endif()

set(tiny_shared_lib ${tiny_base_filename}_lib)
add_library(${tiny_shared_lib} STATIC 
    ${PLATFORM_SOURCES}
)

target_include_directories(
    ${tiny_shared_lib} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
)

# Don't issue warnings for Skia library code.
target_include_directories(
    ${tiny_shared_lib} SYSTEM PUBLIC
    ${SKIA_INCLUDE_DIR}
)

target_link_libraries(${tiny_shared_lib} PUBLIC ${SKIA_LIB})
if(APPLE)
    target_link_libraries(${tiny_shared_lib} PUBLIC
        "-framework Cocoa"
        "-framework CoreFoundation"
        "-framework CoreText"
        "-framework CoreGraphics"
    )
endif()

# Forward to parent scope.
set(tiny_shared_lib ${tiny_shared_lib} PARENT_SCOPE)