# AAX SDK required.
function(make_tfx_exporter USER_TARGET PRESET_DIR)
    # Requires AAX SDK path.
    if(AAX_SDK_ROOT_DIR STREQUAL "n/a" OR CMAKE_SYSTEM_NAME STREQUAL "iOS") # !!!
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

    # AAX SDK includes
    target_include_directories(${PRESETS_TARGET} SYSTEM PRIVATE 
        ${AAX_SDK_ROOT_DIR}/Interfaces
        ${AAX_SDK_ROOT_DIR}/Interfaces/ACF
    )

    if(APPLE)
        set(AAX_LIB_NAME libAAXLibrary_libcpp.a)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Debug)
        else()
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Release)
        endif()
        find_library(AAX_LIB ${AAX_LIB_NAME} PATHS ${AAX_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
    elseif(WIN32)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(AAX_LIB_NAME AAXLibrary_x64_D)
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Debug)
        else()
            set(AAX_LIB_NAME AAXLibrary_x64)
            set(AAX_LIB_DIR ${AAX_SDK_ROOT_DIR}/Libs/Release)
        endif()
        find_library(AAX_LIB ${AAX_LIB_NAME} PATHS ${AAX_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
    endif()

    target_link_libraries(${PRESETS_TARGET} PRIVATE ${USER_TARGET} ${AAX_LIB})
endfunction()

# VST3 SDK required.
function(make_vstpreset_exporter USER_TARGET PRESET_DIR)
    # 
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

    target_link_libraries(${PRESETS_TARGET} PRIVATE ${USER_TARGET} sdk)
endfunction()