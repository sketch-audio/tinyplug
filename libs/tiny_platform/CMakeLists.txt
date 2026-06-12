# ============================================================================
# tiny_platform — native view, dialogs, paths, window context. Carries the Skia
# dependency and the OS frameworks. Depends on the core library; the core does
# NOT depend on it (OS detection lives in core's <tinyplug/platform_defs.hpp>).
# ============================================================================

set(TINY_PLATFORM_LIB tiny_platform)
add_library(${TINY_PLATFORM_LIB} STATIC)

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(PLATFORM_SOURCES
            source/ios_context.mm
            source/ios_dialogs.mm
            source/ios_paths.mm
            source/ios_view.mm
        )
    else()
        # mac_view.mm is compiled per-plugin-format (in each make_xxx_plugin.cmake)
        # so that ObjC class names can be unique per plugin, avoiding runtime
        # collisions when multiple plugins are loaded in the same process.
        set(PLATFORM_SOURCES
            source/mac_context.mm
            source/mac_dialogs.mm
            source/mac_paths.cpp
        )
    endif()
elseif(WIN32)
    set(WIN_GRAPHICS_GPU "0") # GPU renderer not ready for prime time. Need to figure out some wait-free synchronization mechanism.
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/win_config.hpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/win_config.hpp
    )
    target_include_directories(${TINY_PLATFORM_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    set(PLATFORM_SOURCES
        source/win_context.cpp
        source/win_dialogs.cpp
        source/win_paths.cpp
        source/win_view.cpp
    )
endif()

target_sources(${TINY_PLATFORM_LIB} PRIVATE
    ${PLATFORM_SOURCES}
    source/view_delegate.cpp
)

target_sources(${TINY_PLATFORM_LIB} PRIVATE
    include/tiny_platform/tiny_platform.hpp
    include/tiny_platform/platform_dialogs.hpp
    include/tiny_platform/platform_paths.hpp
    include/tiny_platform/platform_view.hpp
    include/tiny_platform/view_delegate.hpp
    include/tiny_platform/window_context.hpp
    source/win_internal.hpp
)

# Public include root → consumers (that link this lib) get <tiny_platform/...>.
target_include_directories(${TINY_PLATFORM_LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Platform depends on core (PUBLIC — its headers expose core types). Skia is an
# implementation detail (no platform header exposes it post-pimpl), so it is
# PRIVATE; the static-lib link still propagates to the final module to resolve
# symbols, but Skia's headers don't leak to consumers. The editor links Skia
# directly (it draws). Frameworks stay PUBLIC so the final module resolves them.
target_link_libraries(${TINY_PLATFORM_LIB} PUBLIC ${TINY_SHARED_LIB})
target_link_libraries(${TINY_PLATFORM_LIB} PRIVATE tiny::skia)
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${TINY_PLATFORM_LIB} PUBLIC
            "-framework Metal"
            "-framework QuartzCore"
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework UniformTypeIdentifiers"
        )
    else()
        target_link_libraries(${TINY_PLATFORM_LIB} PUBLIC
            "-framework Cocoa"
            "-framework CoreFoundation"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework CoreVideo"
            "-framework UniformTypeIdentifiers"
        )
    endif()
elseif(WIN32)
    if (WIN_GRAPHICS_GPU EQUAL 1)
        target_link_libraries(${TINY_PLATFORM_LIB} PUBLIC d3d12.lib dxgi.lib d3dcompiler.lib)
    endif()
endif()

# Forward to parent scope.
set(TINY_PLATFORM_LIB ${TINY_PLATFORM_LIB} PARENT_SCOPE)
