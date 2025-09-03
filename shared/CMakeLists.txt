# --- SKIA ---
# We're building Skia from iPlug2 using the scripts in the Dependencies folder.
# See: https://github.com/iPlug2/iPlug2

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
    set(PLATFORM_SOURCES
        platform/mac_view.mm
        skia/mac/GaneshMetalWindowContext_mac.h
        skia/mac/GaneshMetalWindowContext_mac.mm
        skia/mac/MacWindowGLUtils.h
        skia/mac/MacWindowInfo.h
        skia/mac/MetalWindowContext.h
        skia/mac/MetalWindowContext.mm
    )
elseif(WIN32)
    set(PLATFORM_SOURCES
        platform/win_view.cpp
        skia/win/D3D12WindowContext_win.cpp
        skia/win/D3DTestUtils.cpp
        skia/win/D3DTestUtils.h
        skia/win/WindowContextFactory_win.h
    )
endif()

set(TINY_SHARED_LIB tiny_shared_lib)
add_library(${TINY_SHARED_LIB} STATIC
    ${PLATFORM_SOURCES}
    platform/platform_view.h
    platform/platform.h
    platform/view_delegate.h
    skia/DisplayParams.h
    skia/WindowContext.cpp
    skia/WindowContext.h
    tinyplug/tiny_events.h
    tinyplug/tiny_exports.h
    tinyplug/tiny_kernel.h
    tinyplug/tiny_layout.h
    tinyplug/tiny_params.h
    tinyplug/tiny_queue.h
    tinyplug/tiny_utils.h
    tinyplug/tiny_view.h
    tinyplug/tinyplug.h
)

target_include_directories(
    ${TINY_SHARED_LIB} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
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