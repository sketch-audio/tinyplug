# Enable the Audio Unit (v2) SDK. Only need to do this once.
function(enable_auv2_sdk OUT_AUV2_SDK)
    if(NOT APPLE)
        set(${OUT_AUV2_SDK} "n/a" PARENT_SCOPE)
        return()
    endif()

    # ...
    if (TARGET AudioUnitSDK)
        set(${OUT_AUV2_SDK} AudioUnitSDK PARENT_SCOPE)
        return()
    endif()

    # Build the Audio Unit (v2) SDK as an external project.
    include(ExternalProject)

    add_library(AudioUnitSDK INTERFACE)
    set(AUV2_SDK_VERSION 1.3.0)

    set(AUV2_SDK_EXT AudioUnitSDK_Ext)
    set(AUV2_SDK_PROJ ${CMAKE_CURRENT_BINARY_DIR}/${AUV2_SDK_EXT}-prefix/src/${AUV2_SDK_EXT})
    set(AUV2_SDK_DSTROOT ${CMAKE_CURRENT_BINARY_DIR}/AudioUnitSDK)

    # Expected output:
    set(AUV2_SDK_PATH ${AUV2_SDK_DSTROOT}/usr/local) # Append usr/local.
    set(AUV2_SDK_LIB ${AUV2_SDK_PATH}/lib/libAudioUnitSDK.a)
    set(AUV2_SDK_INCLUDE ${AUV2_SDK_PATH}/include)
    
    ExternalProject_Add(${AUV2_SDK_EXT}
        GIT_REPOSITORY https://github.com/apple/AudioUnitSDK.git
        GIT_TAG AudioUnitSDK-${AUV2_SDK_VERSION}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/build_sdk.sh 
            ${AUV2_SDK_PROJ} 
            ${AUV2_SDK_DSTROOT}
            ${AUV2_SDK_LIB}
            ${AUV2_SDK_VERSION}
        INSTALL_COMMAND ""
    )

    target_link_libraries(AudioUnitSDK INTERFACE ${AUV2_SDK_LIB})
    target_include_directories(AudioUnitSDK INTERFACE ${AUV2_SDK_INCLUDE})

    set(${OUT_AUV2_SDK} AudioUnitSDK PARENT_SCOPE)
endfunction()

function(derive_build_number version_string out_var)
    # Split version string into major, minor, patch
    string(REPLACE "." ";" _version_list "${version_string}")
    list(GET _version_list 0 _major)
    list(GET _version_list 1 _minor)
    list(GET _version_list 2 _patch)

    # Calculate integer value
    math(EXPR _int "(${_major} << 16) | (${_minor} << 8) | ${_patch}")

    # Set output variable
    set(${out_var} "${_int}" PARENT_SCOPE)
endfunction()

# Make an AUv2 plug-in from a user target.
function(make_auv2_plugin USER_TARGET AUV2_SDK)
    if(NOT APPLE)
        message(STATUS "[tiny] Skipping AUv2 build.")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    # Additional AUv2 properties.
    read_property(${USER_TARGET} TINY_COMPANY_NAME)
    read_property(${USER_TARGET} TINY_MANUFACTURER_CODE)
    read_property(${USER_TARGET} TINY_PLUGIN_CODE)
    read_property(${USER_TARGET} TINY_AUV2_TYPE)

    derive_build_number(${TINY_VERSION_STRING} TINY_AUV2_BUNDLE_VERSION)
    
    set(AUV2_TARGET ${TINY_BASE_FILENAME}_auv2)
    set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/formats/auv2) # Assumes a certain structure!

    add_library(${AUV2_TARGET} MODULE
        ${SOURCE_DIR}/source/auv2_adapters.h
        ${SOURCE_DIR}/source/auv2_effect.cpp
        ${SOURCE_DIR}/source/auv2_effect.h
        ${SOURCE_DIR}/source/auv2_view_factory.mm
        ${SOURCE_DIR}/source/auv2_view.cpp
        ${SOURCE_DIR}/source/auv2_view.h
    )

    target_link_libraries(${AUV2_TARGET} PRIVATE ${AUV2_SDK})
    target_link_libraries(${AUV2_TARGET} PRIVATE ${USER_TARGET})

    target_link_libraries(${AUV2_TARGET} PRIVATE "-framework Cocoa" "-framework AudioToolbox")
    target_compile_options(${AUV2_TARGET} PRIVATE -Wall -Wextra -pedantic)
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

