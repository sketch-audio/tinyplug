# Contract checks — build a per-plugin executable that checks what the framework requires of a
# plug-in, on its processor and models alone, no host: `<Base>_contract_checks`, registered as the
# CTest `contract.<Base>`. Opt-in: a plug-in's CMakeLists calls this explicitly. Links only against
# the plug-in lib. See tools/contract_checks/README.md.

function(make_contract_checks USER_TARGET)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR EMSCRIPTEN)
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    set(CHECKS_TARGET "${TINY_BASE_FILENAME}_contract_checks")
    set(SOURCE_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    add_executable(${CHECKS_TARGET} ${SOURCE_DIR}/contract_checks.cpp)
    target_link_libraries(${CHECKS_TARGET} PRIVATE ${USER_TARGET})
    if(APPLE)
        # std::format needs macOS 13.3's to_chars; this never ships, so the plug-in's own floor needn't hold.
        target_compile_options(${CHECKS_TARGET} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${CHECKS_TARGET} PRIVATE -mmacosx-version-min=13.3)
    endif()
    if(COMMAND tiny_copy_asan_runtime)
        tiny_copy_asan_runtime(${CHECKS_TARGET})
    endif()
    add_test(NAME contract.${TINY_BASE_FILENAME} COMMAND ${CHECKS_TARGET})
    set_tests_properties(contract.${TINY_BASE_FILENAME} PROPERTIES TIMEOUT 120 LABELS contract)
endfunction()
