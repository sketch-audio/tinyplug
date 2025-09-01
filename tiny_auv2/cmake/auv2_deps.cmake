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
    BUILD_COMMAND ${CMAKE_CURRENT_LIST_DIR}/build_sdk.sh 
        ${AUV2_SDK_PROJ} 
        ${AUV2_SDK_DSTROOT}
        ${AUV2_SDK_LIB}
        ${AUV2_SDK_VERSION}
    INSTALL_COMMAND ""
)

target_link_libraries(AudioUnitSDK INTERFACE ${AUV2_SDK_LIB})
target_include_directories(AudioUnitSDK INTERFACE ${AUV2_SDK_INCLUDE})