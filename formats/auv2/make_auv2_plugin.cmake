# Make an AUv2 plug-in from a user target.
function(make_auv2_plugin USER_TARGET)
    if(NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_PLUGIN_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    # Additional AUv2 properties.
    read_property(${USER_TARGET} TINY_COMPANY_NAME)
    read_property(${USER_TARGET} TINY_MANUFACTURER_CODE)
    read_property(${USER_TARGET} TINY_PLUGIN_CODE)
    read_property(${USER_TARGET} TINY_AUV2_TYPE)

    derive_build_number(${TINY_VERSION_STRING} TINY_AUV2_BUNDLE_VERSION)
    
    set(AUV2_TARGET ${TINY_BASE_FILENAME}_auv2)
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    configure_preset_list(${USER_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/auv2_preset_list.h)

    add_library(${AUV2_TARGET} MODULE
        ${SOURCE_DIR}/source/auv2_adapters.h
        ${SOURCE_DIR}/source/auv2_effect.cpp
        ${SOURCE_DIR}/source/auv2_effect.h
        ${SOURCE_DIR}/source/auv2_view_factory.mm
        ${SOURCE_DIR}/source/auv2_view.cpp
        ${SOURCE_DIR}/source/auv2_view.h
    )

    target_link_libraries(${AUV2_TARGET} PRIVATE tiny::ausdk)
    target_link_libraries(${AUV2_TARGET} PRIVATE ${USER_TARGET})

    target_link_libraries(${AUV2_TARGET} PRIVATE "-framework Cocoa")
    target_compile_options(${AUV2_TARGET} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wswitch-enum -Wswitch-default -Wshadow)
    target_link_options(${AUV2_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")

    # Configure Info.plist 
    set(TINY_AUDIO_COMPONENT_TAGS "<string>${TINY_AUV2_BUNDLE_TAG}</string>")
    set(TINY_AUV2_BUNDLE_IDENTIFIER "${TINY_BASE_IDENTIFIER}.component")
    math(EXPR TINY_BUNDLE_VERSION_INT "${TINY_AUV2_BUNDLE_VERSION}" OUTPUT_FORMAT DECIMAL)
    configure_file(
        ${SOURCE_DIR}/cmake/Info.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
    )

    # Packaging.
    set(AUV2_BUNDLE_OUTPUT_DIR $<TARGET_BUNDLE_DIR:${AUV2_TARGET}>)
    set_target_properties(${AUV2_TARGET} PROPERTIES
        BUNDLE True
        OUTPUT_NAME ${TINY_BASE_FILENAME}
        BUNDLE_EXTENSION component
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
    )

    # Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
    add_custom_command(
        TARGET ${AUV2_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PkgInfo ${AUV2_BUNDLE_OUTPUT_DIR}/Contents
        VERBATIM
    )

    # We need to copy the native presets into the bundle Resources folder.
    read_property(${USER_TARGET} TINY_PRESET_EXTENSION)
    read_property(${USER_TARGET} TINY_NATIVE_PRESETS_DIR)
    copy_presets(
        ${AUV2_TARGET}
        ".${TINY_PRESET_EXTENSION}"
        ${TINY_NATIVE_PRESETS_DIR}
        "${AUV2_BUNDLE_OUTPUT_DIR}/Contents/Resources"
    )

    read_property(${USER_TARGET} TINY_RESOURCE_LIST)
        if (TINY_RESOURCE_LIST)
            copy_file_list(
                ${AUV2_TARGET}
                "${TINY_RESOURCE_LIST}"
                "${AUV2_BUNDLE_OUTPUT_DIR}/Contents/Resources"
            )
        endif()

    if(TINY_INSTALL_PLUGINS)
        # It seems auval can only find the plug-in if it is in the system library.
        set(AUV2_INSTALL_DIR $ENV{HOME}/Library/Audio/Plug-Ins/Components/)
        add_custom_command(
            TARGET ${AUV2_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${AUV2_BUNDLE_OUTPUT_DIR}" "${AUV2_INSTALL_DIR}/${TINY_BASE_FILENAME}.component"
            COMMENT "Copying AUv2 plugin ${TINY_BASE_FILENAME}.component to ${AUV2_INSTALL_DIR}"
            VERBATIM
        )
    endif()
endfunction()

