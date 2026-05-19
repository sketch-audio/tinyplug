# Make a CLAP plug-in from a user target.
function(make_clap_plugin USER_TARGET)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    set(CLAP_TARGET ${TINY_BASE_FILENAME}_clap)
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    add_library(${CLAP_TARGET} MODULE
        ${SOURCE_DIR}/source/clap_adapters.h
        ${SOURCE_DIR}/source/clap_entry.cpp
        ${SOURCE_DIR}/source/clap_plugin.cpp
        ${SOURCE_DIR}/source/clap_plugin.h
        ${SOURCE_DIR}/source/clap_view.cpp
        ${SOURCE_DIR}/source/clap_view.h
    )

    target_link_libraries(${CLAP_TARGET} PRIVATE tiny::clap tiny::clap-helpers)
    target_link_libraries(${CLAP_TARGET} PRIVATE ${USER_TARGET})

    if(APPLE)
        target_link_libraries(${CLAP_TARGET} PRIVATE "-framework Cocoa")
        target_compile_options(${CLAP_TARGET} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wswitch-enum -Wswitch-default -Wshadow)
        target_link_options(${CLAP_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")

        configure_mac_view(${CLAP_TARGET} ${TINY_BASE_FILENAME} ${TINY_VERSION_STRING} ${TINY_BUILD_NUMBER})
    elseif(WIN32)
        target_compile_options(${CLAP_TARGET} PRIVATE /W3) # Weirdly was getting warnings from the clap-helpers headers at W4.
        target_compile_definitions(${CLAP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    # Packaging.
    if(APPLE)
        set(CLAP_BUNDLE_OUTPUT_DIR $<TARGET_BUNDLE_DIR:${CLAP_TARGET}>)
        set_target_properties(${CLAP_TARGET} PROPERTIES
            BUNDLE True
            OUTPUT_NAME ${TINY_BASE_FILENAME}
            BUNDLE_EXTENSION clap
            MACOSX_BUNDLE_GUI_IDENTIFIER ${TINY_BASE_IDENTIFIER}.clap
            MACOSX_BUNDLE_BUNDLE_NAME ${TINY_PRODUCT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${TINY_BUILD_NUMBER}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${TINY_VERSION_STRING}
            MACOSX_BUNDLE_INFO_PLIST ${SOURCE_DIR}/cmake/Info.plist.in
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.clap
        )

        # Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
        add_custom_command(
            TARGET ${CLAP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PkgInfo ${CLAP_BUNDLE_OUTPUT_DIR}/Contents
            VERBATIM
        )

        # All we need are the native presets.
        read_property(${USER_TARGET} TINY_PRESET_EXTENSION)
        read_property(${USER_TARGET} TINY_NATIVE_PRESETS_DIR)
        copy_presets(
            ${CLAP_TARGET}
            ".${TINY_PRESET_EXTENSION}"
            ${TINY_NATIVE_PRESETS_DIR}
            "${CLAP_BUNDLE_OUTPUT_DIR}/Contents/Resources"
        )

        read_property(${USER_TARGET} TINY_RESOURCE_LIST)
        if (TINY_RESOURCE_LIST)
            copy_file_list(
                ${CLAP_TARGET}
                "${TINY_RESOURCE_LIST}"
                "${CLAP_BUNDLE_OUTPUT_DIR}/Contents/Resources"
            )
        endif()

        if(TINY_INSTALL_PLUGINS)
            # User library so we don't have to elevate.
            set(CLAP_INSTALL_DIR $ENV{HOME}/Library/Audio/Plug-Ins/CLAP)
            add_custom_command(
                TARGET ${CLAP_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAP_INSTALL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${CLAP_BUNDLE_OUTPUT_DIR}" "${CLAP_INSTALL_DIR}/${TINY_BASE_FILENAME}.clap"
                COMMENT "Copying CLAP plugin ${TINY_BASE_FILENAME}.clap to ${CLAP_INSTALL_DIR}"
                VERBATIM
            )
        endif()
    elseif(WIN32)
        # On CLAP we create a pseudo-bundle directory structure matching the VST3 spec.
        set(CLAP_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TINY_BASE_FILENAME}.clap)
        set_target_properties(${CLAP_TARGET} PROPERTIES
            OUTPUT_NAME ${TINY_BASE_FILENAME}
            SUFFIX .clap
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CLAP_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CLAP_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
        )

        # All we need are the native presets.
        read_property(${USER_TARGET} TINY_PRESET_EXTENSION)
        read_property(${USER_TARGET} TINY_NATIVE_PRESETS_DIR)
        copy_presets(
            ${CLAP_TARGET}
            ".${TINY_PRESET_EXTENSION}"
            ${TINY_NATIVE_PRESETS_DIR}
            "${CLAP_BUNDLE_OUTPUT_DIR}/Contents/Resources"
        )

        read_property(${USER_TARGET} TINY_RESOURCE_LIST)
        if (TINY_RESOURCE_LIST)
            copy_file_list(
                ${CLAP_TARGET}
                "${TINY_RESOURCE_LIST}"
                "${CLAP_BUNDLE_OUTPUT_DIR}/Contents/Resources"
            )
        endif()

        add_custom_command(
            TARGET ${CLAP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PlugIn.ico ${CLAP_BUNDLE_OUTPUT_DIR}
        )
        add_custom_command(
            TARGET ${CLAP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/desktop.ini ${CLAP_BUNDLE_OUTPUT_DIR}
        )
        add_custom_command(
            TARGET ${CLAP_TARGET} POST_BUILD 
            COMMAND attrib +s ${CLAP_BUNDLE_OUTPUT_DIR}
        )

        if(TINY_INSTALL_PLUGINS)
        set(CLAP_INSTALL_DEST "$ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP\\${TINY_BASE_FILENAME}.clap")
            add_custom_command(
                TARGET ${CLAP_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CLAP_BUNDLE_OUTPUT_DIR}"
                    "${CLAP_INSTALL_DEST}"
                COMMAND attrib +s "${CLAP_INSTALL_DEST}"
                COMMENT "Copying CLAP plugin to $ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP"
                VERBATIM
            )
        endif()
    endif()
endfunction()

