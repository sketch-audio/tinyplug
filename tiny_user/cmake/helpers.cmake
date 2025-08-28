function(prepare_clap_feature_list list_var_name out_count_var out_array_var)
    # Get the list content by indirect reference
    set(input_list "${${list_var_name}}")

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