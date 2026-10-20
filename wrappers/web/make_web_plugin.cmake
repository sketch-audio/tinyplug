set(TINY_WEB_DIR ${CMAKE_CURRENT_LIST_DIR})

# The web format: `<Base>.web/` with the processor module (and, later, the controller module).
function(make_web_plugin USER_TARGET)
    if(NOT EMSCRIPTEN)
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    set(PROC ${TINY_BASE_FILENAME}_web_processor)
    set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TINY_BASE_FILENAME}.web)

    # Standalone (no Emscripten JS) on imported shared memory: the worklet instantiates it, the pump maps its memory.
    # No -pthread at link: it starts no threads, and standalone wasm can't carry pthreads.
    add_executable(${PROC} ${TINY_WEB_DIR}/source/processor/module.cpp ${TINY_WEB_DIR}/source/wire_layout.hpp)
    target_link_libraries(${PROC} PRIVATE ${USER_TARGET} ${TINY_DSP_LIB})
    target_link_options(${PROC} PRIVATE
        -sSTANDALONE_WASM -sSHARED_MEMORY -sIMPORTED_MEMORY --no-entry
        -sINITIAL_MEMORY=32MB -sMAXIMUM_MEMORY=32MB -sSTACK_SIZE=256KB)
    set_target_properties(${PROC} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}_processor
        SUFFIX ".wasm"
        RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})
endfunction()
