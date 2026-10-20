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

    # The controller: an ordinary Emscripten pthreads module on the main thread. The pool is prewarmed, since a
    # thread started in a browser doesn't run until the main thread yields.
    set(CTL ${TINY_BASE_FILENAME}_web_controller)
    add_executable(${CTL} ${TINY_WEB_DIR}/source/controller/controller.cpp ${TINY_WEB_DIR}/source/manifest.hpp)
    target_link_libraries(${CTL} PRIVATE ${USER_TARGET} ${TINY_PLATFORM_LIB} tiny::skia)
    target_link_options(${CTL} PRIVATE
        -pthread -sPTHREAD_POOL_SIZE=4 -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB
        -sFORCE_FILESYSTEM -lidbfs.js # Platform_paths' folders persist in IndexedDB (controller.js mounts them).
        -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString,stringToNewUTF8,FS,IDBFS)
    set_target_properties(${CTL} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}_controller
        SUFFIX ".mjs"
        RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

    if(TINY_BUILD_TESTS)
        add_test(NAME web_host_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/hosts/web_host.mjs ${OUT_DIR})
        set_tests_properties(web_host_${TINY_BASE_FILENAME} PROPERTIES LABELS web_host)
        add_test(NAME web_split_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/hosts/web_split_host.mjs ${OUT_DIR})
        set_tests_properties(web_split_${TINY_BASE_FILENAME} PROPERTIES LABELS web_split TIMEOUT 60)
        add_test(NAME web_editor_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/web/smoke.mjs ${TINY_BASE_FILENAME}
                 --build ${PROJECT_BINARY_DIR} --page editor.html --result tiny_editor)
        set_tests_properties(web_editor_${TINY_BASE_FILENAME} PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endfunction()
