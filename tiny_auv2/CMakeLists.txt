if(NOT APPLE)
    message(STATUS "This is not a Mac. Skipping AUv2 build.")
    return()
endif()

# AUv2 target
set(AUV2_TARGET ${TINY_BASE_FILENAME}_auv2)

add_library(${AUV2_TARGET} MODULE
    source/auv2_effect.cpp
    source/auv2_view_factory.mm

    # AudioUnitSDK
    sdk/src/AudioUnitSDK/AUBase.cpp
    sdk/src/AudioUnitSDK/AUBuffer.cpp
    sdk/src/AudioUnitSDK/AUBufferAllocator.cpp
    sdk/src/AudioUnitSDK/AUEffectBase.cpp
    sdk/src/AudioUnitSDK/AUInputElement.cpp
    sdk/src/AudioUnitSDK/AUMIDIBase.cpp
    sdk/src/AudioUnitSDK/AUMIDIEffectBase.cpp
    sdk/src/AudioUnitSDK/AUOutputElement.cpp
    sdk/src/AudioUnitSDK/AUPluginDispatch.cpp
    sdk/src/AudioUnitSDK/AUScopeElement.cpp
    sdk/src/AudioUnitSDK/ComponentBase.cpp
    sdk/src/AudioUnitSDK/MusicDeviceBase.cpp
)

target_include_directories(${AUV2_TARGET} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/
)

target_link_libraries(${AUV2_TARGET} PRIVATE ${TINY_SHARED_LIB})
target_link_libraries(${AUV2_TARGET} PRIVATE "-framework Cocoa" "-framework AudioToolbox")

target_compile_options(${AUV2_TARGET} PRIVATE -Wall -Wextra -pedantic)
target_link_options(${AUV2_TARGET} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.txt")

# AudioComponents
set(MACOSX_BUNDLE_COPYRIGHT ${TINY_COMPANY_NAME})
set(MACOSX_BUNDLE_DESCRIPTION ${TINY_PRODUCT_NAME})
set(MACOSX_BUNDLE_TAGS "<string>Effect</string>")
set(MACOSX_BUNDLE_MANUFACTURER ${TINY_AUV2_MANUFACTURER})
set(MACOSX_BUNDLE_TYPE ${TINY_AUV2_TYPE})
set(MACOSX_BUNDLE_SUBTYPE ${TINY_AUV2_SUBTYPE})
math(EXPR MACOSX_BUNDLE_VERSION_INT "0x10600" OUTPUT_FORMAT DECIMAL)

# Packaging.
set(AUV2_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TINY_BASE_FILENAME}.component)
set_target_properties(${AUV2_TARGET} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${TINY_BASE_FILENAME}
    BUNDLE_EXTENSION component
    MACOSX_BUNDLE_GUI_IDENTIFIER ${TINY_BASE_IDENTIFIER}.component
    MACOSX_BUNDLE_BUNDLE_NAME ${TINY_PRODUCT_NAME}
    MACOSX_BUNDLE_BUNDLE_VERSION ${TINY_BUILD_NUMBER}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${TINY_VERSION_STRING}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
)

# Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
add_custom_command(
    TARGET ${AUV2_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${AUV2_BUNDLE_OUTPUT_DIR}/Contents
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