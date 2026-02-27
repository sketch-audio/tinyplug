# Add a custom property to a target.
macro(add_property target property value)
    set_property(TARGET ${target} PROPERTY ${property} ${value})
endmacro()

# Read a custom property on a target into a variable of the same name.
macro(read_property target property)
    get_target_property(${property} ${target} ${property})
endmacro()

# Prepare a the feature list for a CLAP plug-in.
function(prepare_clap_feature_list list_var_name can_process_mono out_count_var out_array_var)
    list(APPEND ${list_var_name} "stereo") # Always have stereo.

    # If can process mono, append "mono" to the list.
    if(can_process_mono STREQUAL "true")
        list(APPEND ${list_var_name} "mono")
    endif()
     
    # Get the list content by indirect reference, replacing semicolons with spaces.
    string(REPLACE ";" " " input_list "${${list_var_name}}")
    
    # Get length of the list
    list(LENGTH "${list_var_name}" list_length)
    math(EXPR list_length_plus_one "${list_length} + 1")

    # Quote each item
    set(quoted_list "")
    foreach(item IN LISTS "${list_var_name}")
        list(APPEND quoted_list "\"${item}\"")
    endforeach()

    # Join with commas
    string(JOIN ", " joined_list ${quoted_list})

    # Return values
    set(${out_count_var} ${list_length_plus_one} PARENT_SCOPE)
    set(${out_array_var} "${joined_list}, nullptr" PARENT_SCOPE)
endfunction()

# Prepare UID array for a VST3 plug-in.
function(prepare_vst3_uid_array item1 item2 item3 item4 out_array_var)
    # Validate length for each item
    foreach(item IN ITEMS item1 item2 item3 item4)
        string(LENGTH "${${item}}" len)
        if(NOT len EQUAL 4)
            message(FATAL_ERROR "Item '${${item}}' must be exactly 4 characters long.")
        endif()
    endforeach()

    # Wrap in single quotes and join
    set(joined "'${item1}', '${item2}', '${item3}', '${item4}'")
    set(${out_array_var} "${joined}" PARENT_SCOPE)
endfunction()

# Configure a plug-in's `plug_info.h` header.
function(configure_plug_info plugin_target output)
    # Extract target properties.
    read_property(${plugin_target} TINY_COMPANY_NAME)
    read_property(${plugin_target} TINY_COMPANY_WEBSITE)
    read_property(${plugin_target} TINY_COMPANY_EMAIL)

    read_property(${plugin_target} TINY_FRAMEWORK_CODE)
    read_property(${plugin_target} TINY_MANUFACTURER_CODE)
    read_property(${plugin_target} TINY_PLUGIN_CODE)

    read_property(${plugin_target} TINY_PRODUCT_NAME)
    read_property(${plugin_target} TINY_PLUGIN_NAME)
    read_property(${plugin_target} TINY_PLUGIN_SHORT_NAME)

    read_property(${plugin_target} TINY_BASE_FILENAME)
    read_property(${plugin_target} TINY_BASE_IDENTIFIER)
    read_property(${plugin_target} TINY_VERSION_STRING)
    read_property(${plugin_target} TINY_BUILD_NUMBER)

    read_property(${plugin_target} TINY_PLUGIN_WANTS_SIDECHAIN)
    read_property(${plugin_target} TINY_PLUGIN_CAN_PROCESS_MONO)

    read_property(${plugin_target} TINY_CLAP_DESCRIPTION)
    read_property(${plugin_target} TINY_CLAP_FEATURES)

    read_property(${plugin_target} TINY_AUV2_VIEW_CLASS)
    read_property(${plugin_target} TINY_AUV2_TYPE)
    read_property(${plugin_target} TINY_AUV2_BUNDLE_VERSION)
    read_property(${plugin_target} TINY_AUV2_BUNDLE_TAG)

    read_property(${plugin_target} TINY_VST3_SUBCATEGORIES)

    # Preset stuff, set sensible defaults.
    read_property(${plugin_target} TINY_COMPANY_DIRECTORY_NAME)
    if (NOT TINY_COMPANY_DIRECTORY_NAME)
        set(TINY_COMPANY_DIRECTORY_NAME ${TINY_COMPANY_NAME})
    endif()
    read_property(${plugin_target} TINY_PRODUCT_DIRECTORY_NAME)
    if (NOT TINY_PRODUCT_DIRECTORY_NAME)
        set(TINY_PRODUCT_DIRECTORY_NAME ${TINY_PRODUCT_NAME})
    endif()

    read_property(${plugin_target} TINY_PRESET_VERSION)
    if (NOT TINY_PRESET_VERSION)
        set(TINY_PRESET_VERSION "0")
    endif()
    read_property(${plugin_target} TINY_PRESET_EXTENSION)
    if (NOT TINY_PRESET_EXTENSION)
        set(TINY_PRESET_EXTENSION "json")
    endif()

    # So use a define for AUv3
    if(TINY_PLUGIN_WANTS_SIDECHAIN STREQUAL "true")
        set(TINY_WANTS_SIDECHAIN 1)
    else()
        set(TINY_WANTS_SIDECHAIN 0)
    endif()

    # Generate the CLAP feature list.
    prepare_clap_feature_list(
        TINY_CLAP_FEATURES ${TINY_PLUGIN_CAN_PROCESS_MONO}
        TINY_CLAP_FEATURE_COUNT TINY_CLAP_FEATURE_VALUES
    )

    # Generate the VST3 UID arrays.
    prepare_vst3_uid_array(
        ${TINY_FRAMEWORK_CODE} ${TINY_MANUFACTURER_CODE} ${TINY_PLUGIN_CODE} "ctrl"
        TINY_VST3_CONTROLLER_UID_LITERALS
    )
    prepare_vst3_uid_array(
        ${TINY_FRAMEWORK_CODE} ${TINY_MANUFACTURER_CODE} ${TINY_PLUGIN_CODE} "proc"
        TINY_VST3_PROCESSOR_UID_LITERALS
    )

    # Configure the plug_info.h file.
    configure_file(
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plug_info.h.in
        ${output}
    )
endfunction()

# Generate build number for AUv2 and AUv3 target plists.
function(derive_build_number version_string out_var)
    # Split version string into major, minor, patch
    string(REPLACE "." ";" _version_list "${version_string}")
    list(GET _version_list 0 _major)
    list(GET _version_list 1 _minor)
    list(GET _version_list 2 _patch)

    # Calculate integer value
    math(EXPR _int "(${_major} << 16) | (${_minor} << 8) | ${_patch}")

    # Set output variable
    set(${out_var} "${_int}" PARENT_SCOPE)
endfunction()

function(configure_preset_list AU_TARGET OUTPUT_FILE)
    read_property(${AU_TARGET} TINY_PRESET_EXTENSION)
    read_property(${AU_TARGET} TINY_NATIVE_PRESETS_DIR)
    read_property(${AU_TARGET} TINY_FIRST_PRESET_NAME)

    set(PRESET_COUNT 0)
    set(PRESET_NAMES "")

    if (TINY_NATIVE_PRESETS_DIR AND EXISTS ${TINY_NATIVE_PRESETS_DIR})
        file(GLOB PRESET_FILES "${TINY_NATIVE_PRESETS_DIR}/*.${TINY_PRESET_EXTENSION}")
        list(SORT PRESET_FILES)

        # If a first preset name is specified, move it to the front of the list.
        if (TINY_FIRST_PRESET_NAME)
            set(FIRST_PRESET_PATH "${TINY_NATIVE_PRESETS_DIR}/${TINY_FIRST_PRESET_NAME}.${TINY_PRESET_EXTENSION}")
            if (EXISTS "${FIRST_PRESET_PATH}")
                list(REMOVE_ITEM PRESET_FILES "${FIRST_PRESET_PATH}")
                list(INSERT PRESET_FILES 0 "${FIRST_PRESET_PATH}")
            endif()
        endif()

        list(LENGTH PRESET_FILES PRESET_COUNT)

        if (PRESET_COUNT GREATER 0)
            foreach(FILE_NAME ${PRESET_FILES})
                get_filename_component(BASE_NAME "${FILE_NAME}" NAME_WE)
                string(APPEND PRESET_NAMES "\"${BASE_NAME}\"")
                list(GET PRESET_FILES -1 LAST_FILE)
                if(NOT FILE_NAME STREQUAL LAST_FILE)
                    string(APPEND PRESET_NAMES ", ")
                endif()
            endforeach()
        endif()
    endif()

    configure_file(
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/preset_list.h.in
        ${OUTPUT_FILE}
        @ONLY
    )
endfunction()

function(copy_presets TARGET EXTENSION SOURCE_DIR DEST_DIR)
    # Check if the directory exists to avoid GLOB errors
    if(EXISTS "${SOURCE_DIR}")
        # Find all files with the specified extension
        file(GLOB PRESET_FILES "${SOURCE_DIR}/*${EXTENSION}")

        foreach(PRESET_FILE ${PRESET_FILES})
            # Get just the filename (e.g., "Crunchy.tfx")
            get_filename_component(PRESET_FILENAME "${PRESET_FILE}" NAME)

            # Register the copy command
            add_custom_command(
                TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DEST_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PRESET_FILE}"
                    "${DEST_DIR}/${PRESET_FILENAME}"
                COMMENT "Copying preset ${PRESET_FILENAME} to ${DEST_DIR}"
                VERBATIM
            )
        endforeach()
    else()
        message(STATUS "Note: Preset source directory not found: ${SOURCE_DIR}")
    endif()
endfunction()

function(copy_file_list TARGET FILE_LIST DEST_DIR)
    foreach(FILE_PATH IN LISTS FILE_LIST)
        get_filename_component(FILE_NAME "${FILE_PATH}" NAME)

        add_custom_command(
            TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DEST_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FILE_PATH}"
                "${DEST_DIR}/${FILE_NAME}"
            COMMENT "Copying file ${FILE_NAME} to ${DEST_DIR}"
            VERBATIM
        )
    endforeach()
endfunction()