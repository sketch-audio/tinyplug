# Some of the plug-in information has to be defined in CMake.
set(tiny_product_name "Tiny Demo")
set(tiny_base_filename "TinyDemo")
set(tiny_base_identifier "com.sketch-audio.TinyDemo")
set(tiny_version_string "1.0.0")
set(tiny_build_number "1")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake_defines.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_defines.h
)

# macOS-specific settings
set(CMAKE_MACOSX_RPATH 1)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# Set include and library directories
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/skia/build/include)
set(LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/skia/build/mac/lib/Release)
find_library(SKIA_LIB skia PATHS ${LIB_DIR} NO_DEFAULT_PATH REQUIRED)
include_directories(${INCLUDE_DIR})

set(tiny_shared_lib ${tiny_base_filename}_lib)
add_library(
    ${tiny_shared_lib} STATIC 
    tinyplug.h
    user_plug.h
    platform/mac_view.mm
)

target_include_directories(
    ${tiny_shared_lib} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${INCLUDE_DIR}
)

target_link_libraries(${tiny_shared_lib} PUBLIC ${SKIA_LIB})
target_link_libraries(
    ${tiny_shared_lib} PUBLIC
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-framework CoreText"
    "-framework CoreGraphics"
)

# Forward to parent scope.
set(tiny_product_name ${tiny_product_name} PARENT_SCOPE)
set(tiny_base_filename ${tiny_base_filename} PARENT_SCOPE)
set(tiny_base_identifier ${tiny_base_identifier} PARENT_SCOPE)
set(tiny_version_string ${tiny_version_string} PARENT_SCOPE)
set(tiny_build_number ${tiny_build_number} PARENT_SCOPE)
set(tiny_shared_lib ${tiny_shared_lib} PARENT_SCOPE)