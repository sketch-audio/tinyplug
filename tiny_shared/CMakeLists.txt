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

set(tiny_shared_lib ${tiny_base_filename}_lib)
add_library(
    ${tiny_shared_lib} INTERFACE
    tinyplug.h
    user_plug.h
)

target_include_directories(
    ${tiny_shared_lib} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Forward to parent scope.
set(tiny_product_name ${tiny_product_name} PARENT_SCOPE)
set(tiny_base_filename ${tiny_base_filename} PARENT_SCOPE)
set(tiny_base_identifier ${tiny_base_identifier} PARENT_SCOPE)
set(tiny_version_string ${tiny_version_string} PARENT_SCOPE)
set(tiny_build_number ${tiny_build_number} PARENT_SCOPE)
set(tiny_shared_lib ${tiny_shared_lib} PARENT_SCOPE)