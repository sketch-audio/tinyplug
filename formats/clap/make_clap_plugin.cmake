# Enable the CLAP SDK. Only need to do this once.
function(enable_clap_sdk CLAP_SDK_VER OUT_CLAP_SDK OUT_CLAP_HELPERS)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(${OUT_CLAP_SDK} "n/a" PARENT_SCOPE)
        set(${OUT_CLAP_HELPERS} "n/a" PARENT_SCOPE)
        return()
    endif()

    include(FetchContent)

    FetchContent_Declare(
        clap
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG ${CLAP_SDK_VER}
        FIND_PACKAGE_ARGS NAMES clap
    )

    FetchContent_Declare(
        clap-helpers
        GIT_REPOSITORY https://github.com/free-audio/clap-helpers.git
        GIT_TAG main # clap-helpers not tagged...
        FIND_PACKAGE_ARGS NAMES clap-helpers
    )

    # Suppress a CLAP dependency (dev) warning.
    if(WIN32)
        cmake_policy(SET CMP0177 NEW)
        set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "No dev warnings")
    endif()

    FetchContent_MakeAvailable(clap clap-helpers)

    set(${OUT_CLAP_SDK} clap PARENT_SCOPE)
    set(${OUT_CLAP_HELPERS} clap-helpers PARENT_SCOPE)
endfunction()

# Make a CLAP plug-in from a user target.
function(make_clap_plugin USER_TARGET CLAP_SDK CLAP_HELPERS)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    set(CLAP_TARGET ${TINY_BASE_FILENAME}_clap)
    set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/formats/clap) # Assumes a certain structure!

    add_library(${CLAP_TARGET} MODULE
        ${SOURCE_DIR}/source/clap_adapters.h
        ${SOURCE_DIR}/source/clap_entry.cpp
        ${SOURCE_DIR}/source/clap_kernel.cpp
        ${SOURCE_DIR}/source/clap_kernel.h
        ${SOURCE_DIR}/source/clap_plugin.cpp
        ${SOURCE_DIR}/source/clap_plugin.h
        ${SOURCE_DIR}/source/clap_view.cpp
        ${SOURCE_DIR}/source/clap_view.h
    )

    # Don't issue warnings for clap library code.
    get_target_property(CLAP_SOURCE_DIR ${CLAP_SDK} SOURCE_DIR)
    get_target_property(CLAP_HELPERS_SOURCE_DIR ${CLAP_HELPERS} SOURCE_DIR)
    target_include_directories(${CLAP_TARGET} SYSTEM PRIVATE
        ${CLAP_SOURCE_DIR}/include
        ${CLAP_HELPERS_SOURCE_DIR}/include
    )

    target_link_libraries(${CLAP_TARGET} PRIVATE ${CLAP_SDK} ${CLAP_HELPERS})
    target_link_libraries(${CLAP_TARGET} PRIVATE ${USER_TARGET})

    if(APPLE)
        target_link_libraries(${CLAP_TARGET} PRIVATE "-framework Cocoa")
        target_compile_options(${CLAP_TARGET} PRIVATE -Wall -Wextra -pedantic)
        target_link_options(${CLAP_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")
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
            MACOSX_BUNDLE_INFO_PLIST ${SOURCE_DIR}/cmake/Info.plist.in
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.clap
        )

        # Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
        add_custom_command(
            TARGET ${CLAP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SOURCE_DIR}/cmake/PkgInfo ${CLAP_BUNDLE_OUTPUT_DIR}/Contents
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
endfunction()

