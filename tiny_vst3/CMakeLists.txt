cmake_minimum_required(VERSION 3.25)

project(
    tiny_vst3
    VERSION 1.0
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64") # !!!

# Set product name and base identifier.
set(product_name TinyDemo)
set(product_identifier "com.sketch-audio.TinyDemo")


# VST3 target.
set(vst3_target ${product_name}_vst3)

# The VST3 SDK apparently needs one of these to be defined.
if (CMAKE_BUILD_TYPE MATCHES Release)
    add_compile_definitions(NDEBUG RELEASE)
else()
    add_compile_definitions(_DEBUG DEVELOPMENT)
endif()

include(cmake/vst3_deps.cmake)
smtg_enable_vst3_sdk() # sdk

add_library(${vst3_target} MODULE
    source/cids.h
    source/controller.h
    source/controller.cpp
    source/entry.cpp
    source/processor.h
    source/processor.cpp
)

target_sources(${vst3_target} PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp)
target_link_libraries(${vst3_target} PRIVATE sdk)
target_link_libraries(${vst3_target} PRIVATE "-framework Foundation")
target_compile_options(${vst3_target} PRIVATE -Wall -Wextra -pedantic)


# Packaging.
set_target_properties(${vst3_target} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${product_name}
    BUNDLE_EXTENSION vst3
    MACOSX_BUNDLE_GUI_IDENTIFIER ${product_identifier}.vst3
    MACOSX_BUNDLE_BUNDLE_NAME ${product_name}
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
)

# Move from the lib folder to the root of the build directory.
set(output_dir ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
add_custom_command(TARGET ${vst3_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${output_dir}/${product_name}.vst3 ${CMAKE_BINARY_DIR}/${product_name}.vst3
    VERBATIM
)

# Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
add_custom_command(TARGET ${vst3_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmake/PkgInfo ${CMAKE_BINARY_DIR}/${product_name}.vst3/Contents
    VERBATIM
)