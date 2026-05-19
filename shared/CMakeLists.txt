set(TINY_SHARED_LIB tiny_shared_lib)
add_library(${TINY_SHARED_LIB} STATIC)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.zip
    URL_HASH SHA256=34660b5e9a407195d55e8da705ed26cc6d175ce5a6b1fb957e701fb4d5b04022
)
FetchContent_MakeAvailable(nlohmann_json)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/win_config.h.in 
        ${CMAKE_CURRENT_BINARY_DIR}/win_config.h
    )
    target_include_directories(${TINY_SHARED_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    set(PLATFORM_SOURCES
        platform/source/win_context.cpp
        platform/source/win_dialogs.cpp
        platform/source/win_paths.cpp
        platform/source/win_view.cpp
    )
endif()

target_sources(${TINY_SHARED_LIB} PRIVATE
    ${PLATFORM_SOURCES}
    platform/source/view_delegate.cpp
    tinyplug/action_queue.cpp
    tinyplug/gesture_recognizers.cpp
    tinyplug/notification_queue.cpp
    tinyplug/serial_queue.cpp
    tinyplug/state_adapter.cpp
    tinyplug/task_launcher.cpp
    tinyplug/task_manager.cpp
    tinyplug/undo_history.cpp
)

target_sources(${TINY_SHARED_LIB} PRIVATE 
    platform/platform_dialogs.h
    platform/platform_view.h
    platform/platform.h
    platform/view_delegate.h
    platform/window_context.h
    tinyplug/tiny_events.h
    tinyplug/tiny_meters.h
    tinyplug/tiny_params.h
    tinyplug/tiny_processor.h
    tinyplug/lock_free_queue.hpp
    tinyplug/tiny_utils.h
    tinyplug/tiny_view.h
    tinyplug/tinyplug.h
)

target_include_directories(
    ${TINY_SHARED_LIB} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TINY_SHARED_LIB} PUBLIC tiny::skia nlohmann_json::nlohmann_json)
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${TINY_SHARED_LIB} PUBLIC
            "-framework Metal"
            "-framework QuartzCore"
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework UniformTypeIdentifiers"
        )
    else()
        target_link_libraries(${TINY_SHARED_LIB} PUBLIC
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
        target_link_libraries(${TINY_SHARED_LIB} PUBLIC d3d12.lib dxgi.lib d3dcompiler.lib)
    endif()
endif()

# Forward to parent scope.
set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)