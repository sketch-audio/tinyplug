#!/bin/bash

AUV2_SDK_PROJ="$1"
AUV2_SDK_DSTROOT="$2"
AUV2_SDK_LIB="$3"
AUV2_SDK_VERSION="$4"

AUV2_SDK_VERSION_FILE="${AUV2_SDK_DSTROOT}/.sdk_version"

NEED_BUILD=0

# Check if lib exists
if [ ! -f "${AUV2_SDK_LIB}" ]; then
    NEED_BUILD=1
fi

# Check if version changed
if [ -f "${AUV2_SDK_VERSION_FILE}" ]; then
    LAST_VERSION=$(cat "${AUV2_SDK_VERSION_FILE}")
    if [ "${LAST_VERSION}" != "${AUV2_SDK_VERSION}" ]; then
        NEED_BUILD=1
    fi
else
    NEED_BUILD=1
fi

# Build if needed
if [ "${NEED_BUILD}" -eq 1 ]; then
    cd "${AUV2_SDK_PROJ}" || exit 1
    xcodebuild \
        -quiet install \
        -configuration Release \
        -sdk macosx \
        DSTROOT="${AUV2_SDK_DSTROOT}" || exit 1
    echo "${AUV2_SDK_VERSION}" > "${AUV2_SDK_VERSION_FILE}"
    echo "Built AudioUnitSDK ${AUV2_SDK_VERSION}."
else
    echo "Already built AudioUnitSDK ${AUV2_SDK_VERSION}."
fi