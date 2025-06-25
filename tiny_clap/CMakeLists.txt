# CLAP target.
set(clap_target ${tiny_base_filename}_clap)

include(cmake/clap_deps.cmake)

add_library(${clap_target} MODULE
    source/clap_entry.cpp
    source/clap_plugin.cpp
)

# Don't issue warnings for clap library code.
target_include_directories(${clap_target} SYSTEM PRIVATE
    ${clap_SOURCE_DIR}/include
    ${clap-helpers_SOURCE_DIR}/include
)

target_link_libraries(${clap_target} PRIVATE clap clap-helpers)
target_link_libraries(${clap_target} PRIVATE ${tiny_shared_lib})

if(APPLE)
    target_link_libraries(${clap_target} PRIVATE "-framework Cocoa")
    target_compile_options(${clap_target} PRIVATE -Wall -Wextra -pedantic)
elseif(WIN32)
endif()

# Packaging.
if(APPLE)
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
elseif(WIN32)
    set_target_properties(${clap_target} PROPERTIES
        OUTPUT_NAME ${tiny_base_filename}
        SUFFIX .clap
    )
    
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/Debug/${tiny_base_filename}.clap" SOURCE_PATH) 
    add_custom_command(
        TARGET ${clap_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP
        COMMAND ${CMAKE_COMMAND} -E copy 
            ${SOURCE_PATH}
            $ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP\\${tiny_base_filename}.clap
        VERBATIM
    )
endif()


