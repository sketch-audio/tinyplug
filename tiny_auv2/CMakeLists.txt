if(NOT APPLE)
    message(STATUS "This is not a Mac. Skipping AUv2 build.")
    return()
endif()

# AUv2 target
set(auv2_target ${tiny_base_filename}_auv2)

add_library(${auv2_target} MODULE
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

target_include_directories(${auv2_target} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/
)

target_link_libraries(${auv2_target} PRIVATE ${tiny_shared_lib})
target_link_libraries(${auv2_target} PRIVATE "-framework Cocoa" "-framework AudioToolbox")

target_compile_options(${auv2_target} PRIVATE -Wall -Wextra -pedantic)
target_link_options(${auv2_target} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.txt")

# AudioComponents
set(MACOSX_BUNDLE_COPYRIGHT ${tiny_manufacturer_name})
set(MACOSX_BUNDLE_DESCRIPTION ${tiny_product_name})
set(MACOSX_BUNDLE_TAGS "<string>Effect</string>")
set(MACOSX_BUNDLE_MANUFACTURER ${tiny_auv2_manufacturer})
set(MACOSX_BUNDLE_TYPE ${tiny_auv2_type})
set(MACOSX_BUNDLE_SUBTYPE ${tiny_auv2_subtype})
math(EXPR MACOSX_BUNDLE_VERSION_INT "0x10600" OUTPUT_FORMAT DECIMAL)

# Packaging.
set(AUV2_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.component)
set_target_properties(${auv2_target} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${tiny_base_filename}
    BUNDLE_EXTENSION component
    MACOSX_BUNDLE_GUI_IDENTIFIER ${tiny_base_identifier}.component
    MACOSX_BUNDLE_BUNDLE_NAME ${tiny_product_name}
    MACOSX_BUNDLE_BUNDLE_VERSION ${tiny_build_number}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${tiny_version_string}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
)

# Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
add_custom_command(
    TARGET ${auv2_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${AUV2_BUNDLE_OUTPUT_DIR}/Contents
    VERBATIM
)

if(TINY_INSTALL_PLUGINS)
    # It seems auval can only find the plug-in if it is in the system library.
    set(AUV2_INSTALL_DIR $ENV{HOME}/Library/Audio/Plug-Ins/Components/)
    add_custom_command(
        TARGET ${auv2_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${AUV2_BUNDLE_OUTPUT_DIR}" "${AUV2_INSTALL_DIR}/${tiny_base_filename}.component"
        COMMENT "Copying AUv2 plugin ${tiny_base_filename}.component to ${AUV2_INSTALL_DIR}"
        VERBATIM
    )
endif()