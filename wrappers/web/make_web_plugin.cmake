set(TINY_WEB_DIR ${CMAKE_CURRENT_LIST_DIR})

# The web format: `<Base>.web/` with the processor module (and, later, the controller module).
function(make_web_plugin USER_TARGET)
    if(NOT EMSCRIPTEN)
        # Natively: the processor module as a renderer, for tests/web/compare.mjs. Built on demand only.
        read_property(${USER_TARGET} TINY_BASE_FILENAME)
        set(NATIVE ${TINY_BASE_FILENAME}_web_native)
        add_executable(${NATIVE} EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/tests/web/native_render.cpp)
        target_link_libraries(${NATIVE} PRIVATE ${USER_TARGET} ${TINY_DSP_LIB})
        target_compile_options(${NATIVE} PRIVATE -ffp-contract=off)
        return()
    endif()

    read_property(${USER_TARGET} TINY_BASE_FILENAME)
    get_target_property(HAS_WORKER ${USER_TARGET} TINY_HAS_WORKER)
    if(HAS_WORKER AND NOT TINY_WEB_VARIANT STREQUAL "isolated")
        message(FATAL_ERROR "[tiny] ${TINY_BASE_FILENAME}: a plug-in with a worker needs TINY_WEB_VARIANT=isolated; "
                            "the nonisolated variant has no threads to run it on.")
    endif()

    set(PROC ${TINY_BASE_FILENAME}_web_processor)
    set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TINY_BASE_FILENAME}.web)

    # Standalone (no Emscripten JS) on imported memory: the worklet instantiates it. Isolated, the memory is shared
    # and the pump maps it; no -pthread at link either way: it starts no threads, and standalone wasm can't carry them.
    set(PROC_MEMORY)
    set(CTL_THREADS)
    if(TINY_WEB_VARIANT STREQUAL "isolated")
        set(PROC_MEMORY -sSHARED_MEMORY)
        # The pool is prewarmed, since a thread started in a browser doesn't run until the main thread yields.
        set(CTL_THREADS -pthread -sPTHREAD_POOL_SIZE=4)
    endif()
    add_executable(${PROC} ${TINY_WEB_DIR}/source/processor/module.cpp ${TINY_WEB_DIR}/source/wire_layout.hpp)
    target_link_libraries(${PROC} PRIVATE ${USER_TARGET} ${TINY_DSP_LIB})
    target_link_options(${PROC} PRIVATE
        -sSTANDALONE_WASM ${PROC_MEMORY} -sIMPORTED_MEMORY --no-entry
        -sINITIAL_MEMORY=32MB -sMAXIMUM_MEMORY=32MB -sSTACK_SIZE=256KB)
    set_target_properties(${PROC} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}_processor
        SUFFIX ".wasm"
        RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

    # The controller: an ordinary Emscripten module on the main thread, with pthreads when isolated.
    set(CTL ${TINY_BASE_FILENAME}_web_controller)
    add_executable(${CTL} ${TINY_WEB_DIR}/source/controller/controller.cpp ${TINY_WEB_DIR}/source/manifest.hpp)
    target_link_libraries(${CTL} PRIVATE ${USER_TARGET} ${TINY_PLATFORM_LIB} tiny::skia)
    target_link_options(${CTL} PRIVATE
        ${CTL_THREADS} -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB
        -sFORCE_FILESYSTEM -lidbfs.js # Platform_paths' folders persist in IndexedDB (controller.js mounts them).
        -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString,stringToNewUTF8,FS,IDBFS)
    set_target_properties(${CTL} PROPERTIES
        OUTPUT_NAME ${TINY_BASE_FILENAME}_controller
        SUFFIX ".mjs"
        RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

    # A self-contained bundle: the two modules, manifest.json, the JS package and a page, deployable as one folder.
    read_property(${USER_TARGET} TINY_PLUGIN_NAME)
    configure_file(${TINY_WEB_DIR}/bundle/index.html.in ${OUT_DIR}/index.html @ONLY)
    file(GLOB TINY_WEB_JS ${TINY_WEB_DIR}/js/*.js)
    find_program(TINY_NODE node REQUIRED)
    add_custom_command(TARGET ${CTL} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}/js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TINY_WEB_JS} ${OUT_DIR}/js
        COMMAND ${TINY_NODE} ${PROJECT_SOURCE_DIR}/tools/web/manifest.mjs $<TARGET_FILE:${PROC}> ${OUT_DIR}/manifest.json
        VERBATIM)
    add_dependencies(${CTL} ${PROC})

    if(TINY_BUILD_TESTS)
        # Nonisolated: the split host over the link, and browser pages served without COOP / COEP.
        set(SPLIT_ARGS)
        set(PAGE_ARGS)
        if(TINY_WEB_VARIANT STREQUAL "nonisolated")
            set(SPLIT_ARGS --link)
            set(PAGE_ARGS --no-isolation)
        endif()
        add_test(NAME web_host_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/hosts/web_host.mjs ${OUT_DIR})
        set_tests_properties(web_host_${TINY_BASE_FILENAME} PROPERTIES LABELS web_host)
        add_test(NAME web_split_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/hosts/web_split_host.mjs ${OUT_DIR} ${SPLIT_ARGS})
        set_tests_properties(web_split_${TINY_BASE_FILENAME} PROPERTIES LABELS web_split TIMEOUT 60)
        add_test(NAME web_editor_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/web/smoke.mjs ${TINY_BASE_FILENAME}
                 --build ${PROJECT_BINARY_DIR} --page editor.html --result tiny_editor ${PAGE_ARGS})
        set_tests_properties(web_editor_${TINY_BASE_FILENAME} PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
        # Against the native build of the same module: configure with -DTINY_WEB_NATIVE_DIR=<a native build tree>
        # where `<Base>_web_native` is built. Exact unless the plug-in sets TINY_WEB_COMPARE_DB (libm differs).
        if(TINY_WEB_NATIVE_DIR)
            file(RELATIVE_PATH REL ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR})
            get_target_property(TOLERANCE ${USER_TARGET} TINY_WEB_COMPARE_DB)
            set(TOLERANCE_ARGS)
            if(TOLERANCE)
                set(TOLERANCE_ARGS --tolerance ${TOLERANCE})
            endif()
            add_test(NAME web_native_${TINY_BASE_FILENAME} COMMAND node ${PROJECT_SOURCE_DIR}/tests/web/compare.mjs $<TARGET_FILE:${PROC}>
                     --native-exe ${TINY_WEB_NATIVE_DIR}/${REL}/${TINY_BASE_FILENAME}_web_native ${TOLERANCE_ARGS})
            set_tests_properties(web_native_${TINY_BASE_FILENAME} PROPERTIES LABELS web_native)
        endif()
    endif()
endfunction()
