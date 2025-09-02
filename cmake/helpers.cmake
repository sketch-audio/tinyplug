# Add a custom property to a target.
macro(add_property target property value)
    set_property(TARGET ${target} PROPERTY ${property} ${value})
endmacro()

# Read a custom property on a target into a variable of the same name.
macro(read_property target property)
    get_target_property(${property} ${target} ${property})
endmacro()

# Prepare a the feature list for a CLAP plug-in.
function(prepare_clap_feature_list list_var_name out_count_var out_array_var)
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
    read_property(${plugin_target} TINY_PRODUCT_SHORT_NAME)
    read_property(${plugin_target} TINY_BASE_FILENAME)
    read_property(${plugin_target} TINY_BASE_IDENTIFIER)
    read_property(${plugin_target} TINY_VERSION_STRING)
    read_property(${plugin_target} TINY_BUILD_NUMBER)

    read_property(${plugin_target} TINY_PLUGIN_WANTS_SIDECHAIN)

    read_property(${plugin_target} TINY_CLAP_DESCRIPTION)
    read_property(${plugin_target} TINY_CLAP_FEATURES)

    read_property(${plugin_target} TINY_AUV2_VIEW_CLASS)
    read_property(${plugin_target} TINY_AUV2_TYPE)
    read_property(${plugin_target} TINY_AUV2_BUNDLE_VERSION)
    read_property(${plugin_target} TINY_AUV2_BUNDLE_TAG)

    read_property(${plugin_target} TINY_VST3_SUBCATEGORIES)

    # Generate the CLAP feature list.
    prepare_clap_feature_list(
        TINY_CLAP_FEATURES 
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
        ${CMAKE_SOURCE_DIR}/cmake/plug_info.h.in
        ${output}
    )
endfunction()