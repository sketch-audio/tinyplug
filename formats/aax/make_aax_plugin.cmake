# Make an AAX plug-in from a user target.
function(make_aax_plugin USER_TARGET AAX_SDK_ROOT_DIR)
    # Requires AAX SDK path.
    if(AAX_SDK_ROOT_DIR STREQUAL "n/a" OR CMAKE_SYSTEM_NAME STREQUAL "iOS") # !!!
        return()
    endif()

    read_property(${USER_TARGET} TINY_AAX_CATEGORIES)
    # Configure header aax_categories.h.in
    configure_file(
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/aax_categories.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/aax_categories.h
    )

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    set(AAX_TARGET ${TINY_BASE_FILENAME}_aax)
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    add_library(${AAX_TARGET} MODULE
        ${SOURCE_DIR}/source/aax_adapters.h
        ${SOURCE_DIR}/source/aax_describe.cpp
        ${SOURCE_DIR}/source/aax_gui.cpp
        ${SOURCE_DIR}/source/aax_gui.h
        ${SOURCE_DIR}/source/aax_monolith.cpp
        ${SOURCE_DIR}/source/aax_monolith.h
        ${SOURCE_DIR}/source/aax_parameters.cpp
        ${SOURCE_DIR}/source/aax_parameters.h
        ${SOURCE_DIR}/source/aax_taper_delegate.h
    )

    # AAX SDK sources
    target_sources(${AAX_TARGET} PRIVATE ${AAX_SDK_ROOT_DIR}/Interfaces/AAX_Exports.cpp)

    # AAX SDK includes
    target_include_directories(${AAX_TARGET} SYSTEM PRIVATE 
        ${AAX_SDK_ROOT_DIR}/Interfaces
        ${AAX_SDK_ROOT_DIR}/Interfaces/ACF
    )

    if(APPLE)
        set(AAX_LIB_NAME libAAXLibrary_libcpp.a)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Debug)
        else()
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Release)
        endif()
        find_library(AAX_LIB ${AAX_LIB_NAME} PATHS ${AAX_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
    elseif(WIN32)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(AAX_LIB_NAME AAXLibrary_x64_D)
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Debug)
        else()
            set(AAX_LIB_NAME AAXLibrary_x64)
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Release)
        endif()
        find_library(AAX_LIB ${AAX_LIB_NAME} PATHS ${AAX_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
    endif()

    target_link_libraries(${AAX_TARGET} PRIVATE ${AAX_LIB})
    target_link_libraries(${AAX_TARGET} PRIVATE ${USER_TARGET})

    if(APPLE)
        target_link_libraries(${AAX_TARGET} PRIVATE "-framework Cocoa")
        target_compile_options(${AAX_TARGET} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wswitch-enum -Wswitch-default -Wshadow)
        target_link_options(${AAX_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")
    elseif(WIN32)
        target_compile_options(${AAX_TARGET} PRIVATE /W4)
    endif()

    # Packaging.
    if(APPLE)
        # Set up the bundle.
        set(AAX_BUNDLE_OUTPUT_DIR $<TARGET_BUNDLE_DIR:${AAX_TARGET}>)
        set_target_properties(${AAX_TARGET} PROPERTIES
            BUNDLE True
            OUTPUT_NAME ${TINY_BASE_FILENAME}
            BUNDLE_EXTENSION aaxplugin 
            MACOSX_BUNDLE_GUI_IDENTIFIER ${TINY_BASE_IDENTIFIER}.aaxplugin
            MACOSX_BUNDLE_BUNDLE_NAME ${TINY_PRODUCT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${TINY_BUILD_NUMBER}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${TINY_VERSION_STRING}
            MACOSX_BUNDLE_INFO_PLIST ${SOURCE_DIR}/cmake/Info.plist.in
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.aaxplugin
        )
        
        # Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
        add_custom_command(
            TARGET ${AAX_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PkgInfo ${AAX_BUNDLE_OUTPUT_DIR}/Contents
            VERBATIM
        )

        if(TINY_INSTALL_PLUGINS)
            set(AAX_INSTALL_DIR "/Library/Application Support/Avid/Audio/Plug-Ins")
            add_custom_command(
                TARGET ${AAX_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${AAX_BUNDLE_OUTPUT_DIR}" "${AAX_INSTALL_DIR}/${TINY_BASE_FILENAME}.aaxplugin"
                COMMENT "Copying AAX plugin ${TINY_BASE_FILENAME}.aaxplugin to ${AAX_INSTALL_DIR}"
                VERBATIM
            )
        endif()
    elseif(WIN32)
        # Set up the bundle.
        set(AAX_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TINY_BASE_FILENAME}.aaxplugin)
        set_target_properties(${AAX_TARGET} PROPERTIES
            OUTPUT_NAME ${TINY_BASE_FILENAME}
            SUFFIX .aaxplugin
            LIBRARY_OUTPUT_DIRECTORY ${AAX_BUNDLE_OUTPUT_DIR}/Contents/x64
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${AAX_BUNDLE_OUTPUT_DIR}/Contents/x64
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${AAX_BUNDLE_OUTPUT_DIR}/Contents/x64
        )

        # Get icon file from the SDK.
        add_custom_command(
            TARGET ${AAX_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AAX_SDK_ROOT_DIR}/Utilities/PlugIn.ico ${AAX_BUNDLE_OUTPUT_DIR}
        )
        add_custom_command(
            TARGET ${AAX_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/desktop.ini ${AAX_BUNDLE_OUTPUT_DIR}
        )

        # Add system attribute. (Causes File Explorer to use the custom icon.)
        add_custom_command(
            TARGET ${AAX_TARGET} POST_BUILD 
            COMMAND attrib +s ${AAX_BUNDLE_OUTPUT_DIR}
        )

        if(TINY_INSTALL_PLUGINS)
            # Copy plugin to Avid's system folder (will ask for admin rights)
            add_custom_command(
                TARGET ${AAX_TARGET} POST_BUILD
                COMMAND "${SOURCE_DIR}/cmake/postbuild_copy.bat" "${AAX_BUNDLE_OUTPUT_DIR}"
                COMMENT "Copying AAX plugin to system folder (will prompt for administrator)"
                VERBATIM
            )
        endif()
    endif()
endfunction()

