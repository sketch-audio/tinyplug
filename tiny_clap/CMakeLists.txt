# CLAP target.
set(CLAP_TARGET ${TINY_BASE_FILENAME}_clap)

include(cmake/clap_deps.cmake)

file(GLOB TARGET_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.h"
)

add_library(${CLAP_TARGET} MODULE
    ${TARGET_HEADERS}
    source/clap_entry.cpp
    source/clap_kernel.cpp
    source/clap_plugin.cpp
    source/clap_view.cpp
)

# Don't issue warnings for clap library code.
target_include_directories(${CLAP_TARGET} SYSTEM PRIVATE
    ${clap_SOURCE_DIR}/include
    ${clap-helpers_SOURCE_DIR}/include
)

target_link_libraries(${CLAP_TARGET} PRIVATE clap clap-helpers)
target_link_libraries(${CLAP_TARGET} PRIVATE ${TINY_USER_CODE})

if(APPLE)
    target_link_libraries(${CLAP_TARGET} PRIVATE "-framework Cocoa")
    target_compile_options(${CLAP_TARGET} PRIVATE -Wall -Wextra -pedantic)
    target_link_options(${CLAP_TARGET} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.txt")
elseif(WIN32)
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
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.clap
    )

    # Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
    add_custom_command(
        TARGET ${CLAP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${CLAP_BUNDLE_OUTPUT_DIR}/Contents
        VERBATIM
    )

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
    set_target_properties(${CLAP_TARGET} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}
        SUFFIX .clap
    )

    if(TINY_INSTALL_PLUGINS)
        add_custom_command(
            TARGET ${CLAP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP"
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE_DIR:${CLAP_TARGET}>\\${TINY_BASE_FILENAME}.clap"
                "$ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP\\${TINY_BASE_FILENAME}.clap"
            COMMENT "Copying CLAP plugin to $ENV{LOCALAPPDATA}\\Programs\\Common\\CLAP"
            VERBATIM
        )
    endif()
endif()


