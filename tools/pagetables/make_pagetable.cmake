# Pagetable manifest — build a per-plugin executable that dumps the plug-in's
# parameter list as JSON. Step 1 of the AAX page-table pipeline; step 2 is
# tools/pagetables/generate_pages.py (JSON -> *Pages.xml). See README in tools/.
# Opt-in: a plug-in's CMakeLists calls this explicitly. No extra SDK required —
# it links only against the plug-in lib.

function(make_pagetable_manifest USER_TARGET)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    set(MANIFEST_TARGET "${TINY_BASE_FILENAME}_pagetable_manifest")
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    add_executable(${MANIFEST_TARGET} ${SOURCE_DIR}/pagetable_manifest.cpp)
    target_link_libraries(${MANIFEST_TARGET} PRIVATE ${USER_TARGET})
endfunction()
