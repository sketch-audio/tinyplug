# Enable the VST3 SDK. Only need to do this once.
function(enable_vst3_sdk OUT_VST3_SDK OUT_VST3_SDK_ROOT_DIR)
    include(FetchContent)

    # https://github.com/kunitoki/yup
    set(SMTG_ADD_VST3_UTILITIES OFF)
    set(SMTG_CREATE_MODULE_INFO OFF)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)
    set(SMTG_RUN_VST_VALIDATOR OFF)

    if(WIN32)
        set(SMTG_USE_STATIC_CRT ON) # We need this for Skia.
    endif()

    FetchContent_Declare(
        vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG v3.7.13_build_42
    )

    FetchContent_MakeAvailable(
        vst3sdk
    )

    # Don't issue warnings when building VST3 sdk.
    target_compile_options(sdk PRIVATE -w)

    # Exclude sdk_hosting and validator from the default build
    set_target_properties(sdk_hosting PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(validator PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # The VST3 SDK apparently needs these to be defined.
    if (CMAKE_BUILD_TYPE MATCHES Release)
        add_compile_definitions(NDEBUG RELEASE)
    else()
        add_compile_definitions(_DEBUG DEVELOPMENT)
    endif()

    smtg_enable_vst3_sdk() # sdk

    set(${OUT_VST3_SDK} sdk PARENT_SCOPE)
    set(${OUT_VST3_SDK_ROOT_DIR} ${vst3sdk_SOURCE_DIR} PARENT_SCOPE)
endfunction()

# Make a VST3 plug-in from a user target.
function(make_vst3_plugin USER_TARGET VST3_SDK VST3_SDK_ROOT_DIR)
    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    read_property(${USER_TARGET} TINY_BASE_IDENTIFIER)
    read_property(${USER_TARGET} TINY_PRODUCT_NAME)
    read_property(${USER_TARGET} TINY_VERSION_STRING)
    read_property(${USER_TARGET} TINY_BUILD_NUMBER)

    set(VST3_TARGET "${TINY_BASE_FILENAME}_vst3")
    set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/formats/vst3) # Assumes a certain structure!

    add_library(${VST3_TARGET} MODULE
        ${SOURCE_DIR}/source/vst3_adapters.h
        ${SOURCE_DIR}/source/vst3_controller.cpp
        ${SOURCE_DIR}/source/vst3_controller.h
        ${SOURCE_DIR}/source/vst3_entry.cpp
        ${SOURCE_DIR}/source/vst3_processor.cpp
        ${SOURCE_DIR}/source/vst3_processor.h
        ${SOURCE_DIR}/source/vst3_view.cpp
        ${SOURCE_DIR}/source/vst3_view.h
    )

    if(APPLE)
        target_sources(${VST3_TARGET} PRIVATE ${VST3_SDK_ROOT_DIR}/public.sdk/source/main/macmain.cpp)
    elseif(WIN32)
        target_sources(${VST3_TARGET} PRIVATE ${VST3_SDK_ROOT_DIR}/public.sdk/source/main/dllmain.cpp)
    endif()

    target_link_libraries(${VST3_TARGET} PRIVATE ${VST3_SDK})
    target_link_libraries(${VST3_TARGET} PRIVATE ${USER_TARGET})

    if(APPLE)
        target_link_libraries(${VST3_TARGET} PRIVATE "-framework Cocoa")
        target_compile_options(${VST3_TARGET} PRIVATE -Wall -Wextra -pedantic)
        target_link_options(${VST3_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")

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

        add_custom_command(
            TARGET ${VST3_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VST3_SDK_ROOT_DIR}/cmake/templates/VST_Logo_Steinberg.ico ${VST3_BUNDLE_OUTPUT_DIR}
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
        endif()
    endif()
endfunction()