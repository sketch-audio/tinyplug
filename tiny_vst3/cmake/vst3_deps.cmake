cmake_minimum_required(VERSION 3.14.0) # Per Steinberg Hello World plug-in

include(FetchContent)

FetchContent_Declare(
    vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG master
)

FetchContent_MakeAvailable(
    vst3sdk
)

# https://github.com/kunitoki/yup
set(SMTG_ADD_VST3_UTILITIES OFF)
set(SMTG_CREATE_MODULE_INFO OFF)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)
set(SMTG_RUN_VST_VALIDATOR OFF)

smtg_enable_vst3_sdk()