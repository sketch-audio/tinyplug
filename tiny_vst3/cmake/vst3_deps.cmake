include(FetchContent)

# https://github.com/kunitoki/yup
set(SMTG_ADD_VST3_UTILITIES OFF)
set(SMTG_CREATE_MODULE_INFO OFF)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)
set(SMTG_RUN_VST_VALIDATOR OFF)

FetchContent_Declare(
    vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.7.13_build_42
)

FetchContent_MakeAvailable(
    vst3sdk
)

# Exclude sdk_hosting and validator from the default build
set_target_properties(sdk_hosting PROPERTIES EXCLUDE_FROM_ALL TRUE)
set_target_properties(validator PROPERTIES EXCLUDE_FROM_ALL TRUE)