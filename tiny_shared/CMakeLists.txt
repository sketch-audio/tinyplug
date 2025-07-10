# Configure generated header.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/plug_info.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/plug_info.h
)

# --- SKIA ---
# We're expecting to use the iPlug2 optional dependencies folder.
# See: https://github.com/iPlug2/iPlug2/releases 

# Platform-specific settings.
if(APPLE)
    set(CMAKE_MACOSX_RPATH 1)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
elseif(WIN32)
    # Windows settings (static CRT) at top level.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN SK_DIRECT3D)
endif()

# Set include and library directories.
if(APPLE)
    set(SKIA_INCLUDE_DIR ${TINY_SKIA_ROOT_DIR}/src/skia)
    set(SKIA_LIB_DIR ${TINY_SKIA_ROOT_DIR}/mac/lib)
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
    file(GLOB PLATFORM_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/skia/mac/*.h")
    set(PLATFORM_SOURCES
        skia/mac/MetalWindowContext.mm
        skia/mac/GaneshMetalWindowContext_mac.mm
        platform/mac_view.mm
    )
elseif(WIN32)
    file(GLOB PLATFORM_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/skia/win/*.h")
    set(PLATFORM_SOURCES
        skia/win/D3DTestUtils.cpp
        skia/win/D3D12WindowContext_win.cpp
        platform/win_view.cpp
    )
endif()

file(GLOB TARGET_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/*.h"
)

set(TINY_SHARED_LIB ${TINY_BASE_FILENAME}_lib)
add_library(${TINY_SHARED_LIB} STATIC 
    ${TARGET_HEADERS}
    skia/WindowContext.cpp
    ${PLATFORM_HEADERS}
    ${PLATFORM_SOURCES}
)

target_include_directories(
    ${TINY_SHARED_LIB} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR} # for plug_info.h
)

# Don't issue warnings for Skia library code.
target_include_directories(
    ${TINY_SHARED_LIB} SYSTEM PUBLIC
    ${SKIA_INCLUDE_DIR}
)

target_link_libraries(${TINY_SHARED_LIB} PUBLIC ${SKIA_LIB})
if(APPLE)
    target_link_libraries(${TINY_SHARED_LIB} PUBLIC
        "-framework Cocoa"
        "-framework CoreFoundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreVideo"
    )
elseif(WIN32)
    target_link_libraries(${TINY_SHARED_LIB} PUBLIC d3d12.lib dxgi.lib d3dcompiler.lib)
endif()

# Forward to parent scope.
set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)