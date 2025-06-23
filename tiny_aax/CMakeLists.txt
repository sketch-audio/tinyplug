# AAX target.

# Requires AAX SDK path.
if(NOT TINY_AAX_SDK_DIR)
    message(STATUS "Set TINY_AAX_SDK_DIR to build AAX plug-ins.")
    return()
endif()

set(aax_target ${tiny_base_filename}_aax)

add_library(${aax_target} MODULE
    source/aax_algorithm.cpp
    source/aax_describe.cpp
    ${TINY_AAX_SDK_DIR}/Interfaces/AAX_Exports.cpp
)

# Probably not all needed.
target_include_directories(
    ${aax_target} SYSTEM PRIVATE 
    ${TINY_AAX_SDK_DIR}/Interfaces
    ${TINY_AAX_SDK_DIR}/Interfaces/ACF
    ${TINY_AAX_SDK_DIR}/Extensions/MIDI
    ${TINY_AAX_SDK_DIR}/Extensions/Platform/include
    ${TINY_AAX_SDK_DIR}/Extensions/Topology
    ${TINY_AAX_SDK_DIR}/TI/SignalProcessing
)

# https://github.com/willpirkleaudio/ASPiK/blob/develop/PluginTemplate/project_source/cmake/aax_cmake/CMakeLists.txt
if(APPLE)
	add_library(aaxLibraryD STATIC IMPORTED)
	set_property(TARGET aaxLibraryD PROPERTY IMPORTED_LOCATION "${TINY_AAX_SDK_DIR}/Libs/Debug/libAAXLibrary_libcpp.a")
	target_link_libraries(${aax_target} PRIVATE debug aaxLibraryD)

	add_library(aaxLibraryR STATIC IMPORTED)
	set_property(TARGET aaxLibraryR PROPERTY IMPORTED_LOCATION ${TINY_AAX_SDK_DIR}/Libs/Release/libAAXLibrary_libcpp.a)
	target_link_libraries(${aax_target} PRIVATE optimized aaxLibraryR)
elseif(WIN)
	add_library(aaxLibraryD STATIC IMPORTED)
	set_property(TARGET aaxLibraryD PROPERTY IMPORTED_LOCATION "${TINY_AAX_SDK_DIR}/Libs/Debug/AAXLibrary_x64_D.lib")
	target_link_libraries(${aax_target} PRIVATE debug aaxLibraryD)

	add_library(aaxLibraryR STATIC IMPORTED)
	set_property(TARGET aaxLibraryR PROPERTY IMPORTED_LOCATION ${TINY_AAX_SDK_DIR}/Libs/Release/AAXLibrary_x64.lib )
	target_link_libraries(${aax_target} PRIVATE optimized aaxLibraryR)
endif()


target_link_libraries(${aax_target} PRIVATE ${tiny_shared_lib})
target_link_libraries(${aax_target} PRIVATE "-framework Cocoa")

target_compile_options(${aax_target} PRIVATE -Wall -Wextra -pedantic)

# Packaging.
set_target_properties(${aax_target} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${tiny_base_filename}
    BUNDLE_EXTENSION aaxplugin 
    MACOSX_BUNDLE_GUI_IDENTIFIER ${tiny_base_identifier}.aaxplugin
    MACOSX_BUNDLE_BUNDLE_NAME ${tiny_product_name}
    MACOSX_BUNDLE_BUNDLE_VERSION ${tiny_build_number}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${tiny_version_string}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
)

# Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
add_custom_command(
    TARGET ${aax_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.aaxplugin/Contents
    VERBATIM
)

# Determine the destination directory based on platform
if(APPLE)
    set(AAX_PLUGIN_DEST_DIR "/Library/Application Support/Avid/Audio/Plug-Ins")
elseif(WIN32)
    set(AAX_PLUGIN_DEST_DIR "$ENV{ProgramFiles}/Common Files/Avid/Audio/Plug-Ins")
else()
    message(WARNING "Unsupported platform for AAX plugin installation")
    return()
endif()

# Add custom command to copy the AAX plugin after build
add_custom_command(
    TARGET ${aax_target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.aaxplugin"
        "${AAX_PLUGIN_DEST_DIR}/${tiny_base_filename}.aaxplugin"
    COMMENT "Copying AAX plugin ${tiny_base_filename}.aaxplugin to ${AAX_PLUGIN_DEST_DIR}"
    VERBATIM
)