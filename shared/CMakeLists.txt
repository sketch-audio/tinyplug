if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(PLATFORM_SOURCES platform/ios_view.mm)
    else()
        set(PLATFORM_SOURCES platform/mac_view.mm)
    endif()
elseif(WIN32)
    set(PLATFORM_SOURCES
        platform/win_view.cpp
    )
endif()

set(TINY_SHARED_LIB tiny_shared_lib)
add_library(${TINY_SHARED_LIB} STATIC
    ${PLATFORM_SOURCES}
    platform/platform_view.h
    platform/platform.h
    platform/view_delegate.h
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

target_link_libraries(${TINY_SHARED_LIB} PUBLIC skia skia-window-contexts) # from tiny_deps
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${TINY_SHARED_LIB} PUBLIC
            "-framework Metal"
            "-framework QuartzCore"
            "-framework UIKit"
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