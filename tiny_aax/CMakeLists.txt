# AAX target.

# Requires AAX SDK path.
if(NOT TINY_AAX_SDK_DIR)
    message(STATUS "TINY_AAX_SDK_DIR is not set. Skipping AAX build.")
    return()
endif()

message(STATUS "Building AAX plug-in...")
message(STATUS "TINY_AAX_SDK_DIR: ${TINY_AAX_SDK_DIR}")

set(aax_target ${tiny_base_filename}_aax)

add_library(${aax_target} MODULE
    source/aax_algorithm.cpp
    source/aax_describe.cpp
)

# AAX SDK sources
target_sources(${aax_target} PRIVATE ${TINY_AAX_SDK_DIR}/Interfaces/AAX_Exports.cpp)

# AAX SDK includes
target_include_directories(${aax_target} SYSTEM PRIVATE 
    ${TINY_AAX_SDK_DIR}/Interfaces
    ${TINY_AAX_SDK_DIR}/Interfaces/ACF
)

if(APPLE)
	add_library(aaxLibraryD STATIC IMPORTED)
	set_property(TARGET aaxLibraryD PROPERTY IMPORTED_LOCATION ${TINY_AAX_SDK_DIR}/Libs/Debug/libAAXLibrary_libcpp.a)
	target_link_libraries(${aax_target} PRIVATE debug aaxLibraryD)

	add_library(aaxLibraryR STATIC IMPORTED)
	set_property(TARGET aaxLibraryR PROPERTY IMPORTED_LOCATION ${TINY_AAX_SDK_DIR}/Libs/Release/libAAXLibrary_libcpp.a)
	target_link_libraries(${aax_target} PRIVATE optimized aaxLibraryR)
elseif(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(AAX_LIB_NAME AAXLibrary_x64_D)
        set(AAX_LIB_DIR ${TINY_AAX_SDK_DIR}/Libs/Debug)
    else()
        set(AAX_LIB_NAME AAXLibrary_x64)
        set(AAX_LIB_DIR ${TINY_AAX_SDK_DIR/Libs/Release})
    endif()
    find_library(AAX_LIB ${AAX_LIB_NAME} PATHS ${AAX_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
endif()

target_link_libraries(${aax_target} PRIVATE ${AAX_LIB})
target_link_libraries(${aax_target} PRIVATE ${tiny_shared_lib})

if(APPLE)
    target_link_libraries(${aax_target} PRIVATE "-framework Cocoa")
    target_compile_options(${aax_target} PRIVATE -Wall -Wextra -pedantic)
elseif(WIN32)
    target_compile_options(${aax_target} PRIVATE /W4)
endif()

if(APPLE)
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
    set(AAX_PLUGIN_DEST_DIR "/Library/Application Support/Avid/Audio/Plug-Ins")

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
elseif(WIN32)
    # Set up the bundle.
    set(AAX_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.aaxplugin)
    set_target_properties(${aax_target} PROPERTIES
        OUTPUT_NAME ${tiny_base_filename}
        SUFFIX .aaxplugin
        LIBRARY_OUTPUT_DIRECTORY ${AAX_BUNDLE_OUTPUT_DIR}/Contents/x64
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${AAX_BUNDLE_OUTPUT_DIR}/Contents/x64
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${AAX_BUNDLE_OUTPUT_DIR}/Contents/x64
    )

    # Get icon file from the SDK.
    add_custom_command(
        TARGET ${aax_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TINY_AAX_SDK_DIR}/Utilities/PlugIn.ico ${AAX_BUNDLE_OUTPUT_DIR}
    )
    add_custom_command(
        TARGET ${aax_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/desktop.ini ${AAX_BUNDLE_OUTPUT_DIR}
    )

    # Add system attribute. (Causes File Explorer to use the custom icon.)
    add_custom_command(
        TARGET ${aax_target} POST_BUILD 
        COMMAND attrib +s ${AAX_BUNDLE_OUTPUT_DIR}
    )

    if(TINY_INSTALL_PLUGINS)
        # Copy plugin to Avid's system folder (will ask for admin rights)
        add_custom_command(
            TARGET ${aax_target} POST_BUILD
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cmake/postbuild_copy.bat" "${AAX_BUNDLE_OUTPUT_DIR}"
            COMMENT "Copying AAX plugin to system folder (will prompt for administrator)"
            VERBATIM
        )
    endif()
endif()