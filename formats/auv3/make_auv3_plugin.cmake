function(make_auv3_plugin USER_TARGET)
    if(NOT APPLE OR NOT CMAKE_GENERATOR STREQUAL "Xcode")
        message(STATUS "[tiny] AUv3 build requires Xcode generator.")
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
    read_property(${USER_TARGET} TINY_COMPANY_WEBSITE)
    read_property(${USER_TARGET} TINY_MANUFACTURER_CODE)
    read_property(${USER_TARGET} TINY_PLUGIN_CODE)
    read_property(${USER_TARGET} TINY_AUV2_TYPE) # same as AUv2
    read_property(${USER_TARGET} TINY_APP_XCASSETS)

    derive_build_number(${TINY_VERSION_STRING} TINY_AUV3_BUNDLE_VERSION)
    read_property(${USER_TARGET} TINY_PLUGIN_WANTS_SIDECHAIN)

    # For entitlements files.
    read_property(${USER_TARGET} TINY_COMPANY_DIRECTORY_NAME)
    read_property(${USER_TARGET} TINY_PRODUCT_DIRECTORY_NAME)

    read_property(${USER_TARGET} TINY_APP_PLIST_ENTRIES)
    if(NOT TINY_APP_PLIST_ENTRIES)
        set(TINY_APP_PLIST_ENTRIES "")
    endif()

    # Optional app info properties (for the standalone app UI).
    read_property(${USER_TARGET} TINY_APP_DESCRIPTION)
    if(NOT TINY_APP_DESCRIPTION)
        set(TINY_APP_DESCRIPTION "")
    endif()
    read_property(${USER_TARGET} TINY_APP_MANUAL_URL)
    if(NOT TINY_APP_MANUAL_URL)
        set(TINY_APP_MANUAL_URL "")
    endif()
    read_property(${USER_TARGET} TINY_APP_SUPPORT_URL)
    if(NOT TINY_APP_SUPPORT_URL)
        set(TINY_APP_SUPPORT_URL "")
    endif()
    read_property(${USER_TARGET} TINY_APP_PRIVACY_URL)
    if(NOT TINY_APP_PRIVACY_URL)
        set(TINY_APP_PRIVACY_URL "")
    endif()
    read_property(${USER_TARGET} TINY_APP_STORE_URL)
    if(NOT TINY_APP_STORE_URL)
        set(TINY_APP_STORE_URL "")
    endif()

    read_property(${USER_TARGET} TINY_AUV3_SIZE_TAG)
    if(TINY_AUV3_SIZE_TAG)
        set(TINY_AUV3_TAGS "<string>${TINY_AUV3_SIZE_TAG}</string>")
    else()
        set(TINY_AUV3_TAGS "")
    endif()
    
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    configure_file(
        ${SOURCE_DIR}/../../cmake/app_info.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/app_info.h
    )

    # See: https://www.jviotti.com/2022/12/21/building-objective-c-ios-apps-with-cmake.html
    set(CMAKE_OBJC_STANDARD 99)
    add_compile_options(
        -fobjc-arc
        #-fobjc-weak
        -fno-common
        -fstrict-aliasing
        -fpascal-strings
        -fmodules)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_definitions(DEBUG=1)
    else()
        add_compile_definitions(NS_BLOCK_ASSERTIONS=1)
        add_compile_options(-fasm-blocks -Os)
    endif()

    set(CMAKE_OSX_SYSROOT iphoneos)

    # App
    # ---
    set(APP_TARGET ${TINY_BASE_FILENAME}_app)

    if (TINY_APP_XCASSETS)
        set(ASSETS_PATH ${TINY_APP_XCASSETS})
    else()
        set(ASSETS_PATH ${SOURCE_DIR}/assets/Assets.xcassets)
    endif()

    add_executable(${APP_TARGET})
    target_sources(${APP_TARGET} PRIVATE
        ${SOURCE_DIR}/source/app/AppDelegate.h
        ${SOURCE_DIR}/source/app/AppDelegate.m
        ${SOURCE_DIR}/source/app/main.m
        ${SOURCE_DIR}/source/app/SceneDelegate.h
        ${SOURCE_DIR}/source/app/SceneDelegate.m
        ${SOURCE_DIR}/source/app/ViewController.h
        ${SOURCE_DIR}/source/app/ViewController.m
        ${SOURCE_DIR}/source/shared/TargetPlatforms.h
        ${ASSETS_PATH}
    )
    set_source_files_properties(${ASSETS_PATH} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )

    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${APP_TARGET} PRIVATE
            "-framework Foundation"
            "-framework UIKit"
        )
        set_target_properties(${APP_TARGET} PROPERTIES
            # Install stuff required so Xcode recognizes this as an app bundle (for distribution).
            XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
            XCODE_ATTRIBUTE_INSTALL_PATH "/Applications"
            XCODE_ATTRIBUTE_DEPLOYMENT_POSTPROCESSING "YES"
            # Strip and generate dSYM for release builds.
            XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS[variant=Release] "YES"
            XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT[variant=Release] "dwarf-with-dsym"
            XCODE_ATTRIBUTE_STRIP_INSTALLED_PRODUCT "YES"
        )
    else()
        target_link_libraries(${APP_TARGET} PRIVATE
            "-framework AppKit"
            "-framework Foundation"
        )
        configure_file(
            ${SOURCE_DIR}/cmake/Entitlements-App.plist.in
            ${CMAKE_CURRENT_BINARY_DIR}/Entitlements-App.plist
        )
        set_target_properties(${APP_TARGET} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_BINARY_DIR}/Entitlements-App.plist"
            # Required (as far as I can tell) for notarization.
            XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "YES"
            XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
            XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS[variant=Release] "NO"
        )
    endif()
    target_include_directories(${APP_TARGET} PUBLIC
        ${SOURCE_DIR}/source/shared
        ${CMAKE_CURRENT_BINARY_DIR} # for app_info.h
    )

    # Configure Info.plist
    set(INFO_APP_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info-App.plist)
    configure_file(
        ${SOURCE_DIR}/cmake/Info-App.plist.in
        ${INFO_APP_PLIST}
    )

    set_target_properties(${APP_TARGET} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}
        MACOSX_BUNDLE YES
        MACOSX_BUNDLE_INFO_PLIST ${INFO_APP_PLIST}
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2" # iPhone and iPad
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
        XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "11.0"
        XCODE_ATTRIBUTE_LAUNCH_SCREEN_STORYBOARD "NO"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_BASE_IDENTIFIER}
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
        XCODE_ATTRIBUTE_ENABLE_TESTABILITY[variant=Debug] "YES"
        XCODE_ATTRIBUTE_COPY_PHASE_STRIP "NO"
        XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator macosx"
        XCODE_ATTRIBUTE_SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD "NO"
        XCODE_ATTRIBUTE_SUPPORTS_XR_DESIGNED_FOR_IPHONE_IPAD "NO"
        XCODE_GENERATE_SCHEME TRUE
    )
    # ---

    # Extension
    # ---
    configure_preset_list(${USER_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/auv3_preset_list.h)

    set(EXT_TARGET ${TINY_BASE_FILENAME}_extension) # AUv3 extension
    add_executable(${EXT_TARGET})
    target_sources(${EXT_TARGET} PRIVATE
        ${SOURCE_DIR}/source/extension/AUProcessHelper.hpp
        ${SOURCE_DIR}/source/extension/auv3_AUAudioUnit.h
        ${SOURCE_DIR}/source/extension/auv3_AUAudioUnit.mm
        ${SOURCE_DIR}/source/extension/auv3_AUViewController.mm
        ${SOURCE_DIR}/source/extension/auv3_view.cpp
        ${SOURCE_DIR}/source/extension/auv3_view.h
        ${SOURCE_DIR}/source/extension/BufferedAudioBus.hpp
        ${SOURCE_DIR}/source/extension/DSPKernel.hpp
        ${SOURCE_DIR}/source/shared/TargetPlatforms.h
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${EXT_TARGET} PRIVATE
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework CoreAudioKit"
            "-framework Foundation"
            "-framework UIKit"
        )
        set_target_properties(${EXT_TARGET} PROPERTIES
            # Strip and generate dSYM for release builds.
            XCODE_ATTRIBUTE_DEPLOYMENT_POSTPROCESSING "YES"
            XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS[variant=Release] "YES"
            XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT[variant=Release] "dwarf-with-dsym"
            XCODE_ATTRIBUTE_STRIP_INSTALLED_PRODUCT "YES"
        )
    else()
        target_link_libraries(${EXT_TARGET} PRIVATE
            "-framework AppKit"
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework CoreAudioKit"
            "-framework Foundation"
        )
        configure_file(
            ${SOURCE_DIR}/cmake/Entitlements-Extension.plist.in
            ${CMAKE_CURRENT_BINARY_DIR}/Entitlements-Extension.plist
        )
        set_target_properties(${EXT_TARGET} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_BINARY_DIR}/Entitlements-Extension.plist"
            # Strip and generate dSYM for release builds.
            XCODE_ATTRIBUTE_DEPLOYMENT_POSTPROCESSING[variant=Release] "YES"
            XCODE_ATTRIBUTE_STRIP_INSTALLED_PRODUCT[variant=Release] "YES"
            XCODE_ATTRIBUTE_STRIP_STYLE[variant=Release] "all" # May not be needed now that we're passing exports.txt
            XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS[variant=Release] "YES"
            XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT[variant=Release] "dwarf-with-dsym"
            # Required (as far as I can tell) for notarization.
            XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "YES"
            XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
            XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS[variant=Release] "NO"
        )
        target_link_options(${EXT_TARGET} PRIVATE "-Wl,-exported_symbols_list,${SOURCE_DIR}/cmake/exports.txt")
    endif()
    target_link_libraries(${EXT_TARGET} PRIVATE ${USER_TARGET})
    target_include_directories(${EXT_TARGET} PRIVATE ${SOURCE_DIR}/source/shared)

    # Configure Info.plist
    set(TINY_AUV3_IDENTIFIER "${TINY_BASE_IDENTIFIER}.auv3") # Must match bundle identifier.
    derive_build_number(${TINY_VERSION_STRING} TINY_AUV3_BUILD_NUMBER)
    set(INFO_EXT_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info-Extension.plist)
    configure_file(
        ${SOURCE_DIR}/cmake/Info-Extension.plist.in
        ${INFO_EXT_PLIST}
    )

    # Get bundle output directory for extension
    read_property(${USER_TARGET} TINY_PRESET_EXTENSION)
    read_property(${USER_TARGET} TINY_NATIVE_PRESETS_DIR)
    # 1. Collect the file paths (manual list is safer, but GLOB works here)
    file(GLOB PRESET_FILES "${TINY_NATIVE_PRESETS_DIR}/*.${TINY_PRESET_EXTENSION}")

    # 2. Tell CMake to put these into the bundle's Resources folder
    target_sources(${EXT_TARGET} PRIVATE ${PRESET_FILES})
    set_source_files_properties(${PRESET_FILES} PROPERTIES 
        MACOSX_PACKAGE_LOCATION Resources
    )

    read_property(${USER_TARGET} TINY_RESOURCE_LIST)
    if(TINY_RESOURCE_LIST)
        target_sources(${EXT_TARGET} PRIVATE ${TINY_RESOURCE_LIST})
        set_source_files_properties(${TINY_RESOURCE_LIST} PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources
        )
    endif()

    # 3. Add them to your target
    target_sources(${EXT_TARGET} PRIVATE ${PRESET_FILES})

    # See: https://developer.apple.com/documentation/xcode/build-settings-reference
    set_target_properties(${EXT_TARGET} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}_AUv3
        BUNDLE_EXTENSION appex
        MACOSX_BUNDLE YES
        MACOSX_BUNDLE_INFO_PLIST ${INFO_EXT_PLIST}
        XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
        XCODE_ATTRIBUTE_EMBEDDED_CONTENT_CONTAINS_SWIFT "NO"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2" # iPhone and iPad
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
        XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "11.0"
        XCODE_ATTRIBUTE_LAUNCH_SCREEN_STORYBOARD "NO"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TINY_AUV3_IDENTIFIER}
        XCODE_ATTRIBUTE_ENABLE_TESTABILITY[variant=Debug] "YES"
        XCODE_ATTRIBUTE_COPY_PHASE_STRIP "NO"
        XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator macosx"
        XCODE_ATTRIBUTE_SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD "NO"
        XCODE_ATTRIBUTE_SUPPORTS_XR_DESIGNED_FOR_IPHONE_IPAD "NO"
        XCODE_ATTRIBUTE_ENABLE_APP_SANDBOX[sdk=macosx*] "YES"
        XCODE_ATTRIBUTE_ENABLE_USER_SELECTED_FILES[sdk=macosx*] "Read-Only"
    )
    # ---

    # Embedding
    set_target_properties(${APP_TARGET} PROPERTIES
        XCODE_EMBED_APP_EXTENSIONS ${EXT_TARGET}
    )
    add_dependencies(${APP_TARGET} ${EXT_TARGET})

endfunction()