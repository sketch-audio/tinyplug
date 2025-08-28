if(NOT APPLE)
    message(STATUS "Platform is not Apple. Skipping AUv2 build.")
    return()
endif()

# AUv2 target
set(AUV2_TARGET ${TINY_BASE_FILENAME}_auv2)

file(GLOB TARGET_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.h"
)

# AudioUnitSDK
file(GLOB SDK_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/AudioUnitSDK/*.h")
file(GLOB SDK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/AudioUnitSDK/*.cpp")

add_library(${AUV2_TARGET} MODULE
    ${TARGET_HEADERS}
    source/auv2_effect.cpp
    source/auv2_view_factory.mm
    source/auv2_view.cpp
    ${SDK_HEADERS}
    ${SDK_SOURCES}
)

target_include_directories(${AUV2_TARGET} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include/
)

target_link_libraries(${AUV2_TARGET} PRIVATE ${TINY_USER_LIB})
target_link_libraries(${AUV2_TARGET} PRIVATE "-framework Cocoa" "-framework AudioToolbox")

target_compile_options(${AUV2_TARGET} PRIVATE -Wall -Wextra -pedantic)
target_link_options(${AUV2_TARGET} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.txt")

# AudioComponents
set(MACOSX_BUNDLE_COPYRIGHT ${TINY_COMPANY_NAME})
set(MACOSX_BUNDLE_DESCRIPTION ${TINY_PRODUCT_NAME})
set(MACOSX_BUNDLE_TAGS "<string>${TINY_AUV2_BUNDLE_TAG}</string>")
set(MACOSX_BUNDLE_MANUFACTURER ${TINY_MANUFACTURER_CODE})
set(MACOSX_BUNDLE_TYPE ${TINY_AUV2_TYPE})
set(MACOSX_BUNDLE_SUBTYPE ${TINY_PLUGIN_CODE})
math(EXPR MACOSX_BUNDLE_VERSION_INT "${TINY_AUV2_BUNDLE_VERSION}" OUTPUT_FORMAT DECIMAL)

# Packaging.
set(AUV2_BUNDLE_OUTPUT_DIR $<TARGET_BUNDLE_DIR:${AUV2_TARGET}>)
set_target_properties(${AUV2_TARGET} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${TINY_BASE_FILENAME}
    BUNDLE_EXTENSION component
    MACOSX_BUNDLE_GUI_IDENTIFIER ${TINY_BASE_IDENTIFIER}.component
    MACOSX_BUNDLE_BUNDLE_NAME ${TINY_PRODUCT_NAME}
    MACOSX_BUNDLE_BUNDLE_VERSION ${TINY_BUILD_NUMBER}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${TINY_VERSION_STRING}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.component
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