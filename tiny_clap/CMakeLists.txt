# CLAP target.
set(clap_target ${tiny_base_filename}_clap)

include(cmake/clap_deps.cmake)

add_library(${clap_target} MODULE
    source/descriptor.h
    source/entry.cpp
    source/plugin.h
    source/plugin.cpp
)

target_include_directories(${clap_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tiny_shared_lib)

target_link_libraries(${clap_target} PRIVATE clap clap-helpers)
target_link_libraries(${clap_target} PRIVATE ${tiny_shared_lib})
target_link_libraries(${clap_target} PRIVATE "-framework Cocoa")

target_compile_options(${clap_target} PRIVATE -Wall -Wextra -pedantic)

# Packaging.
set_target_properties(${clap_target} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${tiny_base_filename}
    BUNDLE_EXTENSION clap
    MACOSX_BUNDLE_GUI_IDENTIFIER ${tiny_base_identifier}.clap
    MACOSX_BUNDLE_BUNDLE_NAME ${tiny_product_name}
    MACOSX_BUNDLE_BUNDLE_VERSION ${tiny_build_number}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${tiny_version_string}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
)

# Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
add_custom_command(
    TARGET ${clap_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.clap/Contents
    VERBATIM
)

# Copy the CLAP bundle to ~/Library/Audio/Plug-Ins/CLAP/ after building.
add_custom_command(
    TARGET ${clap_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $ENV{HOME}/Library/Audio/Plug-Ins/CLAP/
    COMMAND ${CMAKE_COMMAND} -E copy_directory 
        ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.clap 
        $ENV{HOME}/Library/Audio/Plug-Ins/CLAP/${tiny_base_filename}.clap
    VERBATIM
)