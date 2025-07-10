# VST3 target.
set(VST3_TARGET ${TINY_BASE_FILENAME}_vst3)

# The VST3 SDK apparently needs these to be defined.
if (CMAKE_BUILD_TYPE MATCHES Release)
    add_compile_definitions(NDEBUG RELEASE)
else()
    add_compile_definitions(_DEBUG DEVELOPMENT)
endif()

include(cmake/vst3_deps.cmake)
smtg_enable_vst3_sdk() # sdk

file(GLOB TARGET_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.h"
)

add_library(${VST3_TARGET} MODULE
    ${TARGET_HEADERS}
    source/vst3_controller.cpp
    source/vst3_entry.cpp
    source/vst3_processor.cpp
)

if(APPLE)
    target_sources(${VST3_TARGET} PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp)
elseif(WIN32)
    target_sources(${VST3_TARGET} PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp)
endif()

target_link_libraries(${VST3_TARGET} PRIVATE sdk)
target_link_libraries(${VST3_TARGET} PRIVATE ${TINY_SHARED_LIB})

if(APPLE)
    target_link_libraries(${VST3_TARGET} PRIVATE "-framework Cocoa")
    target_compile_options(${VST3_TARGET} PRIVATE -Wall -Wextra -pedantic)
    target_link_options(${VST3_TARGET} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.txt")
elseif(WIN32)
    target_compile_options(${VST3_TARGET} PRIVATE /W4)
endif()


# Packaging.
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
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}.vst3
    )

    # Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
    add_custom_command(
        TARGET ${VST3_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${VST3_BUNDLE_OUTPUT_DIR}/Contents
        VERBATIM
    )

    if(TINY_INSTALL_PLUGINS)
        # User library so we don't have to elevate.
        set(VST3_INSTALL_DIR $ENV{HOME}/Library/Audio/Plug-Ins/VST3)
        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VST3_INSTALL_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${VST3_BUNDLE_OUTPUT_DIR}" "${VST3_INSTALL_DIR}/${TINY_BASE_FILENAME}.vst3"
            COMMENT "Copying VST3 plugin ${TINY_BASE_FILENAME}.vst3 to ${VST3_INSTALL_DIR}"
            VERBATIM
        )
    endif()
elseif(WIN32)
    # Set up the bundle.
    set(VST3_BUNDLE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TINY_BASE_FILENAME}.vst3)
    set_target_properties(${VST3_TARGET} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}
        SUFFIX .vst3
        LIBRARY_OUTPUT_DIRECTORY ${VST3_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${VST3_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${VST3_BUNDLE_OUTPUT_DIR}/Contents/x86_64-win
    )

    # Get icon file from the SDK.
    add_custom_command(
        TARGET ${VST3_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${vst3sdk_SOURCE_DIR}/cmake/templates/VST_Logo_Steinberg.ico ${VST3_BUNDLE_OUTPUT_DIR}
    )
    add_custom_command(
        TARGET ${VST3_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/desktop.ini ${VST3_BUNDLE_OUTPUT_DIR}
    )

    # Add system attribute. (Causes File Explorer to use the custom icon.)
    add_custom_command(
        TARGET ${VST3_TARGET} POST_BUILD 
        COMMAND attrib +s ${VST3_BUNDLE_OUTPUT_DIR}
    )

    if(TINY_INSTALL_PLUGINS)
        # Copy plugin to Common Files\VST3 (will ask for admin rights)
        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cmake/postbuild_copy.bat" "${VST3_BUNDLE_OUTPUT_DIR}"
            COMMENT "Copying VST3 plugin to system folder (will prompt for administrator)"
            VERBATIM
        )
    endif()
endif()