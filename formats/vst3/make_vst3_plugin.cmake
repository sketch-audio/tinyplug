# Make a VST3 plug-in from a user target.
function(make_vst3_plugin USER_TARGET)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    set(VST3_TARGET "${TINY_BASE_FILENAME}_vst3")
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    add_library(${VST3_TARGET} MODULE
        ${SOURCE_DIR}/source/vst3_adapters.h
        ${SOURCE_DIR}/source/vst3_controller.cpp
        ${SOURCE_DIR}/source/vst3_controller.h
        ${SOURCE_DIR}/source/vst3_entry.cpp
        ${SOURCE_DIR}/source/vst3_messaging.cpp
        ${SOURCE_DIR}/source/vst3_messaging.h
        ${SOURCE_DIR}/source/vst3_processor.cpp
        ${SOURCE_DIR}/source/vst3_processor.h
        ${SOURCE_DIR}/source/vst3_view.cpp
        ${SOURCE_DIR}/source/vst3_view.h
    )
    tiny_add_vst3_main(${VST3_TARGET})

    target_link_libraries(${VST3_TARGET} PRIVATE tiny::vst3sdk)
    target_link_libraries(${VST3_TARGET} PRIVATE ${USER_TARGET})

    if(APPLE)
        target_link_libraries(${VST3_TARGET} PRIVATE "-framework Cocoa")
        target_compile_options(${VST3_TARGET} PRIVATE -Wall -Wextra -pedantic -Wconversion -Wswitch-enum -Wswitch-default -Wshadow)
        target_link_options(${VST3_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")

        configure_mac_view(${VST3_TARGET} ${TINY_BASE_FILENAME} ${TINY_VERSION_STRING} ${TINY_BUILD_NUMBER})
    elseif(WIN32)
        target_compile_options(${VST3_TARGET} PRIVATE /W4)
    endif()

    if(APPLE)
        set(VST3_BUNDLE_OUTPUT_DIR $<TARGET_BUNDLE_DIR:${VST3_TARGET}>)
        set_target_properties(${VST3_TARGET} PROPERTIES
            BUNDLE True
            OUTPUT_NAME ${TINY_BASE_FILENAME}
            BUNDLE_EXTENSION vst3
            MACOSX_BUNDLE_GUI_IDENTIFIER ${TINY_BASE_IDENTIFIER}.vst3
            MACOSX_BUNDLE_BUNDLE_NAME ${TINY_PRODUCT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${TINY_BUILD_NUMBER}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${TINY_VERSION_STRING}
            MACOSX_BUNDLE_INFO_PLIST ${SOURCE_DIR}/cmake/Info.plist.in
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.vst3
        )

        # All we need are the native presets. (We need to place the format ones with the installers.)
        read_property(${USER_TARGET} TINY_PRESET_EXTENSION)
        read_property(${USER_TARGET} TINY_NATIVE_PRESETS_DIR)
        copy_presets(
            ${VST3_TARGET}
            ".${TINY_PRESET_EXTENSION}"
            ${TINY_NATIVE_PRESETS_DIR}
            "${VST3_BUNDLE_OUTPUT_DIR}/Contents/Resources"
        )

        read_property(${USER_TARGET} TINY_RESOURCE_LIST)
        if (TINY_RESOURCE_LIST)
            copy_file_list(
                ${VST3_TARGET}
                "${TINY_RESOURCE_LIST}"
                "${VST3_BUNDLE_OUTPUT_DIR}/Contents/Resources"
            )
        endif()

        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PkgInfo ${VST3_BUNDLE_OUTPUT_DIR}/Contents
            VERBATIM
        )

        if(TINY_INSTALL_PLUGINS)
            set(VST3_INSTALL_DIR $ENV{HOME}/Library/Audio/Plug-Ins/VST3)
            add_custom_command(
                TARGET ${VST3_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${VST3_INSTALL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${VST3_BUNDLE_OUTPUT_DIR}" "${VST3_INSTALL_DIR}/${TINY_BASE_FILENAME}.vst3"
                COMMENT "Copying VST3 plugin ${TINY_BASE_FILENAME}.vst3 to ${VST3_INSTALL_DIR}"
                VERBATIM
            )

            # copy format presets to ~/Library/Audio/Presets/Company Name/Product Name
            read_property(${USER_TARGET} TINY_FORMAT_PRESETS_DIR)
            read_property(${USER_TARGET} TINY_COMPANY_NAME)
            read_property(${USER_TARGET} TINY_PLUGIN_NAME)
            copy_presets(
                ${VST3_TARGET}
                ".vstpreset"
                ${TINY_FORMAT_PRESETS_DIR}
                "$ENV{HOME}/Library/Audio/Presets/${TINY_COMPANY_NAME}/${TINY_PLUGIN_NAME}"
            )
        endif()
    elseif(WIN32)
        set(VST3_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TINY_BASE_FILENAME}.vst3)
        set_target_properties(${VST3_TARGET} PROPERTIES
            OUTPUT_NAME ${TINY_BASE_FILENAME}
            SUFFIX .vst3
            LIBRARY_OUTPUT_DIRECTORY ${VST3_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${VST3_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${VST3_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
        )

        # All we need are the native presets. (We need to place the format ones with the installers.)
        read_property(${USER_TARGET} TINY_PRESET_EXTENSION)
        read_property(${USER_TARGET} TINY_NATIVE_PRESETS_DIR)
        copy_presets(
            ${VST3_TARGET}
            ".${TINY_PRESET_EXTENSION}"
            ${TINY_NATIVE_PRESETS_DIR}
            "${VST3_BUNDLE_OUTPUT_DIR}/Contents/Resources"
        )

        read_property(${USER_TARGET} TINY_RESOURCE_LIST)
        if (TINY_RESOURCE_LIST)
            copy_file_list(
                ${VST3_TARGET}
                "${TINY_RESOURCE_LIST}"
                "${VST3_BUNDLE_OUTPUT_DIR}/Contents/Resources"
            )
        endif()

        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PlugIn.ico ${VST3_BUNDLE_OUTPUT_DIR}
        )
        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/desktop.ini ${VST3_BUNDLE_OUTPUT_DIR}
        )
        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD 
            COMMAND attrib +s ${VST3_BUNDLE_OUTPUT_DIR}
        )

        if(TINY_INSTALL_PLUGINS)
            add_custom_command(
                TARGET ${VST3_TARGET} POST_BUILD
                COMMAND "${SOURCE_DIR}/cmake/postbuild_copy.bat" "${VST3_BUNDLE_OUTPUT_DIR}"
                COMMENT "Copying VST3 plugin to system folder (will prompt for administrator)"
                VERBATIM
            )

            # copy format presets to %PROGRAMDATA%\VST3 Presets\Company Name\Product Name
            read_property(${USER_TARGET} TINY_FORMAT_PRESETS_DIR)
            read_property(${USER_TARGET} TINY_COMPANY_NAME)
            read_property(${USER_TARGET} TINY_PLUGIN_NAME)
            copy_presets(
                ${VST3_TARGET}
                ".vstpreset"
                ${TINY_FORMAT_PRESETS_DIR}
                "$ENV{PROGRAMDATA}\\VST3 Presets\\${TINY_COMPANY_NAME}\\${TINY_PLUGIN_NAME}"
            )
        endif()
    endif()
endfunction()