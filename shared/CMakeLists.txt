include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.zip
    URL_HASH SHA256=34660b5e9a407195d55e8da705ed26cc6d175ce5a6b1fb957e701fb4d5b04022
)
FetchContent_MakeAvailable(nlohmann_json)

# ============================================================================
# Core library — format-agnostic framework (params, events, state, tasks, view
# loop). Pure C++: no Skia, no native frameworks. Only forward-declares SkCanvas.
# ============================================================================

set(TINY_SHARED_LIB tiny_shared_lib)
add_library(${TINY_SHARED_LIB} STATIC)

target_sources(${TINY_SHARED_LIB} PRIVATE
    tinyplug/action_queue.cpp
    tinyplug/gesture_recognizers.cpp
    tinyplug/notification_queue.cpp
    tinyplug/serial_queue.cpp
    tinyplug/state_adapter.cpp
    tinyplug/task_launcher.cpp
    tinyplug/task_manager.cpp
    tinyplug/undo_history.cpp
    tinyplug/host_formatter.cpp
    tinyplug/value_helper.cpp
)

target_sources(${TINY_SHARED_LIB} PRIVATE
    tinyplug/tiny_platform.hpp
    tinyplug/tiny_events.hpp
    tinyplug/tiny_meters.hpp
    tinyplug/tiny_params.hpp
    tinyplug/value_helper.hpp
    tinyplug/tiny_processor.hpp
    tinyplug/lock_free_queue.hpp
    tinyplug/tiny_utils.hpp
    tinyplug/tiny_view.hpp
    tinyplug/tiny_worker.hpp
    tinyplug/tinyplug.hpp
)

target_include_directories(${TINY_SHARED_LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${TINY_SHARED_LIB} PUBLIC nlohmann_json::nlohmann_json)

# ============================================================================
# Platform library — native view, dialogs, paths, window context. Carries the
# Skia dependency and the OS frameworks. Depends on the core library; the core
# does NOT depend on it (OS detection lives in core's tiny_platform.hpp).
# ============================================================================

set(TINY_PLATFORM_LIB tiny_platform)
add_library(${TINY_PLATFORM_LIB} STATIC)

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(PLATFORM_SOURCES
            platform/source/ios_context.mm
            platform/source/ios_dialogs.mm
            platform/source/ios_paths.mm
            platform/source/ios_view.mm
        )
    else()
        # mac_view.mm is compiled per-plugin-format (in each make_xxx_plugin.cmake)
        # so that ObjC class names can be unique per plugin, avoiding runtime
        # collisions when multiple plugins are loaded in the same process.
        set(PLATFORM_SOURCES
            platform/source/mac_context.mm
            platform/source/mac_dialogs.mm
            platform/source/mac_paths.cpp
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
        platform/source/win_context.cpp
        platform/source/win_dialogs.cpp
        platform/source/win_paths.cpp
        platform/source/win_view.cpp
    )
endif()

target_sources(${TINY_PLATFORM_LIB} PRIVATE
    ${PLATFORM_SOURCES}
    platform/source/view_delegate.cpp
)

target_sources(${TINY_PLATFORM_LIB} PRIVATE
    platform/platform.hpp
    platform/platform_dialogs.hpp
    platform/platform_paths.hpp
    platform/platform_view.hpp
    platform/view_delegate.hpp
    platform/window_context.hpp
    platform/source/win_internal.hpp
)

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
set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)
set(TINY_PLATFORM_LIB ${TINY_PLATFORM_LIB} PARENT_SCOPE)
