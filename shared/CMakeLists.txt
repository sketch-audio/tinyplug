set(TINY_SHARED_LIB tiny_shared_lib)
add_library(${TINY_SHARED_LIB} STATIC)

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(PLATFORM_SOURCES 
            platform/ios_context.mm
            platform/ios_view.mm
        )
    else()
        # configure file at cmake/mac_config.h.in
        string(REPLACE "." "_" VERSION_STRING "${PROJECT_VERSION}")
        set(TINY_MAC_VIEW "MacView_${PROJECT_NAME}_v${VERSION_STRING}")
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mac_config.h.in 
            ${CMAKE_CURRENT_BINARY_DIR}/mac_config.h
        )
        target_include_directories(${TINY_SHARED_LIB} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
        set(PLATFORM_SOURCES 
            platform/mac_context.mm
            platform/mac_view.mm
        )
    endif()
elseif(WIN32)
    set(PLATFORM_SOURCES
        platform/win_context.cpp
        platform/win_view.cpp
    )
endif()

target_sources(${TINY_SHARED_LIB} PRIVATE
    ${PLATFORM_SOURCES}
    platform/view_delegate.cpp
)

target_sources(${TINY_SHARED_LIB} PRIVATE 
    platform/platform_view.h
    platform/platform.h
    platform/view_delegate.h
    platform/window_context.h
    tinyplug/tiny_events.h
    tinyplug/tiny_meters.h
    tinyplug/tiny_params.h
    tinyplug/tiny_processor.h
    tinyplug/tiny_queue.h
    tinyplug/tiny_utils.h
    tinyplug/tiny_view.h
    tinyplug/tinyplug.h
)

target_include_directories(
    ${TINY_SHARED_LIB} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TINY_SHARED_LIB} PUBLIC skia) # from tiny_deps
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${TINY_SHARED_LIB} PUBLIC
            "-framework Metal"
            "-framework QuartzCore"
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework CoreText"
        )
    else()
        target_link_libraries(${TINY_SHARED_LIB} PUBLIC
            "-framework Cocoa"
            "-framework CoreFoundation"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework CoreVideo"
        )
    endif()
elseif(WIN32)
    target_link_libraries(${TINY_SHARED_LIB} PUBLIC d3d12.lib dxgi.lib d3dcompiler.lib)
endif()

# Forward to parent scope.
set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)