# Preset exporters — build a per-plugin executable that writes factory preset
# files (AAX .tfx / VST3 .vstpreset) from the plug-in's parameter defaults.
# Opt-in: a plug-in's CMakeLists calls these explicitly.

# AAX SDK required.
function(make_tfx_exporter USER_TARGET PRESET_DIR)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    set(PRESETS_TARGET "${TINY_BASE_FILENAME}_tfx_exporter")
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    add_executable(${PRESETS_TARGET}
        ${SOURCE_DIR}/tfx_exporter.cpp
    )

    get_filename_component(PRESET_DIR_ABS ${PRESET_DIR} ABSOLUTE)
    target_compile_definitions(${PRESETS_TARGET} PRIVATE
        PRESET_DIR="${PRESET_DIR_ABS}"
    )

    # The .tfx writer reuses the AAX wrapper's param-ID adapters.
    target_include_directories(${PRESETS_TARGET} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../wrappers/aax/source)
    target_link_libraries(${PRESETS_TARGET} PRIVATE ${USER_TARGET} tiny::aaxsdk)
endfunction()

# VST3 SDK required.
function(make_vstpreset_exporter USER_TARGET PRESET_DIR)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    set(PRESETS_TARGET "${TINY_BASE_FILENAME}_vstpreset_exporter")
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    add_executable(${PRESETS_TARGET}
        ${SOURCE_DIR}/vstpreset_exporter.cpp
    )

    get_filename_component(PRESET_DIR_ABS ${PRESET_DIR} ABSOLUTE)
    target_compile_definitions(${PRESETS_TARGET} PRIVATE
        PRESET_DIR="${PRESET_DIR_ABS}"
    )

    target_link_libraries(${PRESETS_TARGET} PRIVATE ${USER_TARGET} tiny::vst3sdk)
endfunction()
