# ============================================================================
# tests — one executable per file, each registered with CTest. Presets: `tests`, `tsan`, `asan`
# (cmake --preset tests && cmake --build --preset tests && ctest --preset tests).
# ============================================================================

# audio_bench's std::format needs macOS 13.3's to_chars; test executables never ship.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
endif()

include(FetchContent)
FetchContent_Declare(
    audio_bench
    GIT_REPOSITORY https://github.com/sketch-audio/audio_bench.git
    GIT_TAG        0adbf7a177fda9d44f606b3d74aafcfda5799940 # Pinned: bump deliberately.
    SYSTEM
)
FetchContent_MakeAvailable(audio_bench)

find_package(Threads REQUIRED)

add_custom_target(tiny_tests)

# tiny_add_test(<name> [SOURCES ...] [LIBS ...] [INCLUDES ...]); SOURCES defaults to <name>.cpp.
function(tiny_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;INCLUDES" ${ARGN})
    if(NOT T_SOURCES)
        set(T_SOURCES ${name}.cpp)
    endif()
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${TINY_CORE_LIB} audio_bench Threads::Threads ${T_LIBS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${T_INCLUDES})
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion -Wshadow)
    endif()
    tiny_copy_asan_runtime(${name})
    if(EMSCRIPTEN AND TINY_WEB_VARIANT STREQUAL "nonisolated")
        target_link_options(${name} PRIVATE -sEXIT_RUNTIME -sNODERAWFS -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=1MB)
    elseif(EMSCRIPTEN)
        # Under Node: `main` on a pthread so it may block, and the real filesystem.
        target_link_options(${name} PRIVATE -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME -sNODERAWFS -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120) # A hang fails instead of stalling the run.
    add_dependencies(tiny_tests ${name})
endfunction()

# The nonisolated web tree has no threads: its own Task_manager test, and the per-plug-in web tests make_web_plugin
# registers. Everything else here runs in the isolated tree and natively.
set(_nothreads OFF)
if(EMSCRIPTEN AND TINY_WEB_VARIANT STREQUAL "nonisolated")
    set(_nothreads ON)
    tiny_add_test(tasks_nothreads_test)
endif()

set(_standin ${CMAKE_CURRENT_SOURCE_DIR}/support/generated)      # <tiny_models.hpp> with no models.
set(_work_standin ${CMAKE_CURRENT_SOURCE_DIR}/support/work_models) # <tiny_models.hpp> with a work model.
set(_view_standin ${CMAKE_CURRENT_SOURCE_DIR}/support/view_models) # <tiny_models.hpp> with a view model.

if(NOT _nothreads)
# Primitives.
tiny_add_test(lock_free_queue_test)
tiny_add_test(tasks_test)
if(APPLE)
    target_link_libraries(tasks_test PRIVATE "-framework CoreFoundation") # Relay delivers on the main run loop.
endif()
tiny_add_test(change_set_test)
tiny_add_test(data_port_test)
tiny_add_test(worker_runner_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_work_standin})
tiny_add_test(aax_transport_test INCLUDES ${PROJECT_SOURCE_DIR}/wrappers/aax/source)

# Parameters.
tiny_add_test(value_helper_test)
tiny_add_test(params_test)

# Meters, blocks, notes.
tiny_add_test(tiny_meters_test)
tiny_add_test(tiny_blocks_test)
tiny_add_test(notes_test)

# Processor.
tiny_add_test(timeline_resolver_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_standin})
tiny_add_test(stream_sync_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_standin})

# State.
tiny_add_test(state_store_test)
tiny_add_test(state_sequencer_test)
tiny_add_test(state_stress_test)
tiny_add_test(state_retry_test)
tiny_add_test(state_writers_test)
tiny_add_test(state_record_test)
tiny_add_test(state_adapter_test)
target_compile_definitions(state_adapter_test PRIVATE TINY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
tiny_add_test(state_link_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_standin})
tiny_add_test(edit_undo_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_standin})
tiny_add_test(edit_store_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_view_standin})

# Text.
tiny_add_test(tiny_text_test LIBS ${TINY_TEXT_LIB} tiny::skia)
tiny_embed(tiny_text_test NAME fonts FILES
    ${PROJECT_SOURCE_DIR}/examples/text_demo/fonts/InterVariable.ttf
    ${PROJECT_SOURCE_DIR}/examples/text_demo/fonts/InterVariable-Italic.ttf
)
endif()

# The web format's JS: wire.js against <tiny_core/wire.hpp>, across two module instances under Node.
if(EMSCRIPTEN)
    if(NOT _nothreads)
        add_executable(wire_harness web/wire_harness.cpp)
        target_link_libraries(wire_harness PRIVATE ${TINY_CORE_LIB})
        target_link_options(wire_harness PRIVATE -pthread -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=node -sEXPORTED_RUNTIME_METHODS=HEAPU8)
        set_target_properties(wire_harness PROPERTIES SUFFIX ".mjs")
        add_test(NAME wire_js_test COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/web/wire_test.mjs $<TARGET_FILE:wire_harness>)
        add_dependencies(tiny_tests wire_harness)
    endif()
    set(_page_args) # Nonisolated: the same pages, served without COOP / COEP.
    if(_nothreads)
        set(_page_args --no-isolation)
    endif()
    if(TINY_BUILD_PLUGINS AND TARGET GainDemo_web_processor)
        add_test(NAME web_smoke_GainDemo COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/web/smoke.mjs GainDemo --build ${PROJECT_BINARY_DIR} ${_page_args})
        set_tests_properties(web_smoke_GainDemo PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME web_persist_GainDemo COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/web/smoke.mjs GainDemo --build ${PROJECT_BINARY_DIR} --page persist.html --result tiny_persist ${_page_args})
        set_tests_properties(web_persist_GainDemo PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME web_multi COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/web/smoke.mjs GainDemo --build ${PROJECT_BINARY_DIR} --page multi.html --result tiny_multi ${_page_args})
        set_tests_properties(web_multi PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME web_bundle_GainDemo COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/web/smoke.mjs GainDemo --build ${PROJECT_BINARY_DIR}
                 --root ${PROJECT_BINARY_DIR}/examples/gain_demo/GainDemo.web --page index.html --result tiny_ready ${_page_args})
        set_tests_properties(web_bundle_GainDemo PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
    if(TINY_BUILD_PLUGINS AND TARGET StepSequencer_web_processor)
        add_test(NAME web_midi_out COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/web/smoke.mjs StepSequencer --build ${PROJECT_BINARY_DIR}
                 --page editor.html --result tiny_editor --query &play --wait 2000 --expect "tiny_midi > 0" ${_page_args})
        set_tests_properties(web_midi_out PROPERTIES LABELS browser TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()

# ============================================================================
# Fake hosts: real plug-in bundles driven through each format's host API, one CTest per demo.
# Only when the plug-ins are built (the `tsan-hosts` / `asan-hosts` presets).
# ============================================================================

if(TINY_BUILD_PLUGINS AND NOT EMSCRIPTEN)
    add_custom_target(tiny_host_tests)
    # Sessions saved by older builds, which every host must keep loading (fixtures/README.md).
    set(_session_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sessions)
    set(_demos AutomationDemo BlockDemo GainDemo LatencyDemo MeterDemo PlatformDemo SineSynth StateDemo StepSequencer WorkerDemo)

    # Editor support (a window, synthetic input).
    set(_gui_sources)
    if(APPLE)
        set(_gui_sources hosts/gui.mm)
        set_source_files_properties(hosts/gui.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    elseif(WIN32)
        set(_gui_sources hosts/gui_win.cpp)
    endif()

    add_executable(clap_host hosts/clap_host.cpp hosts/support.cpp ${_gui_sources})
    target_link_libraries(clap_host PRIVATE tiny::clap audio_bench Threads::Threads)
    target_include_directories(clap_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hosts)
    if(NOT MSVC)
        target_compile_options(clap_host PRIVATE -Wall -Wextra -Wconversion -Wshadow)
    endif()
    tiny_copy_asan_runtime(clap_host)
    if(APPLE)
        target_link_libraries(clap_host PRIVATE "-framework CoreFoundation -framework AppKit -framework AudioToolbox") # Run loop, editor window.
    endif()
    add_dependencies(tiny_host_tests clap_host)

    if(APPLE OR WIN32)
        add_executable(vst3_host hosts/vst3_host.cpp hosts/support.cpp ${_gui_sources} ${TINY_DEPS_PATH}/third_party/vst3sdk/public.sdk/source/common/memorystream.cpp)
        target_link_libraries(vst3_host PRIVATE sdk_hosting audio_bench Threads::Threads)
        if(APPLE)
            target_link_libraries(vst3_host PRIVATE "-framework CoreFoundation -framework AppKit -framework AudioToolbox")
        endif()
        target_include_directories(vst3_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hosts)
        tiny_copy_asan_runtime(vst3_host)
        add_dependencies(tiny_host_tests vst3_host)
    endif()

    # Each demo's contract checks (tools/contract_checks), no host.
    foreach(demo ${_demos})
        if(NOT TARGET ${demo}_lib)
            continue()
        endif()
        make_contract_checks(${demo}_lib)
        set_tests_properties(contract.${demo} PROPERTIES LABELS "host;contract")
        add_dependencies(tiny_host_tests ${demo}_contract_checks)
    endforeach()

    # AAX: no bundle loading, so one host per demo with the wrapper's sources compiled in.
    if(APPLE OR WIN32)
        set(_aax_source ${PROJECT_SOURCE_DIR}/wrappers/aax/source)
        foreach(demo ${_demos})
            if(NOT TARGET ${demo}_aax)
                continue()
            endif()
            add_executable(aax_host_${demo} hosts/aax_host.cpp hosts/support.cpp ${_gui_sources}
                ${_aax_source}/alg_proc.cpp ${_aax_source}/direct_data.cpp ${_aax_source}/gui.cpp ${_aax_source}/parameters.cpp)
            target_link_libraries(aax_host_${demo} PRIVATE ${demo}_lib tiny::aaxsdk ${TINY_PLATFORM_LIB} ${TINY_DSP_LIB} audio_bench Threads::Threads)
            target_include_directories(aax_host_${demo} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hosts ${_aax_source})
            if(APPLE)
                target_link_libraries(aax_host_${demo} PRIVATE "-framework AppKit -framework CoreFoundation -framework AudioToolbox")
                configure_mac_view(aax_host_${demo} ${demo} ${PROJECT_VERSION} 1 aax_host_${demo})
            endif()
            tiny_copy_asan_runtime(aax_host_${demo})
            add_test(NAME aax_host.${demo} COMMAND aax_host_${demo} --fixtures ${_session_fixtures})
            set_tests_properties(aax_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests aax_host_${demo})
        endforeach()
    endif()

    if(APPLE)
        add_executable(auv2_host hosts/auv2_host.cpp hosts/support.cpp ${_gui_sources})
        target_link_libraries(auv2_host PRIVATE audio_bench Threads::Threads "-framework AudioToolbox -framework CoreFoundation -framework AppKit")
        target_include_directories(auv2_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hosts)
        target_compile_options(auv2_host PRIVATE -Wall -Wextra -Wshadow)
        add_dependencies(tiny_host_tests auv2_host)


        add_executable(auv3_host hosts/auv3_host.mm hosts/support.cpp ${_gui_sources})
        target_compile_options(auv3_host PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        target_link_libraries(auv3_host PRIVATE audio_bench Threads::Threads
            "-framework AppKit -framework AudioToolbox -framework AVFoundation -framework CoreAudioKit -framework Foundation")
        target_include_directories(auv3_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hosts)
        add_dependencies(tiny_host_tests auv3_host)
    endif()

    foreach(demo ${_demos})
        if(TARGET vst3_host AND TARGET ${demo}_vst3)
            if(WIN32) # Not a CMake bundle there; both load the binary directly.
                set(_vst3_path $<TARGET_FILE:${demo}_vst3>)
            else()
                set(_vst3_path $<TARGET_BUNDLE_DIR:${demo}_vst3>)
            endif()
            add_test(NAME vst3_host.${demo} COMMAND vst3_host ${_vst3_path} --fixtures ${_session_fixtures})
            set_tests_properties(vst3_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_vst3)
            if(TARGET validator) # Steinberg's own validator, built from source with the same sanitizer.
                add_test(NAME vst3_validator.${demo} COMMAND validator ${_vst3_path})
                set_tests_properties(vst3_validator.${demo} PROPERTIES TIMEOUT 120 LABELS host)
                if(TINY_ASAN_RUNTIME) # The SDK's target, so the runtime is found on PATH rather than copied beside it.
                    get_filename_component(_asan_dir ${TINY_ASAN_RUNTIME} DIRECTORY)
                    set_tests_properties(vst3_validator.${demo} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_asan_dir}")
                endif()
                add_dependencies(tiny_host_tests validator)
            endif()
        endif()
        if(TARGET auv2_host AND TARGET ${demo}_auv2)
            add_test(NAME auv2_host.${demo} COMMAND auv2_host $<TARGET_BUNDLE_DIR:${demo}_auv2> --fixtures ${_session_fixtures})
            set_tests_properties(auv2_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_auv2)
        endif()
        if(TARGET auv3_host AND TARGET ${demo}_auv3_test AND TARGET ${demo}_auv2)
            add_test(NAME auv3_host.${demo} COMMAND auv3_host $<TARGET_BUNDLE_DIR:${demo}_auv3_test> $<TARGET_BUNDLE_DIR:${demo}_auv2> --fixtures ${_session_fixtures})
            set_tests_properties(auv3_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_auv3_test)
        endif()
        if(TARGET ${demo}_clap)
            if(WIN32) # Not a CMake bundle there; the host finds the bundle from the binary.
                add_test(NAME clap_host.${demo} COMMAND clap_host $<TARGET_FILE:${demo}_clap> --fixtures ${_session_fixtures})
            else()
                add_test(NAME clap_host.${demo} COMMAND clap_host $<TARGET_BUNDLE_DIR:${demo}_clap> --fixtures ${_session_fixtures})
            endif()
            set_tests_properties(clap_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_clap)
        endif()
    endforeach()

    # The native preset exporters (tools/presets), run on a copy of each fixture preset in the build tree;
    # the VST3 and AAX hosts then load what they wrote, as a DAW loads a factory preset.
    if(APPLE OR WIN32)
        foreach(demo StateDemo)
            if(NOT TARGET ${demo}_lib OR NOT TARGET vst3_host OR NOT TARGET aax_host_${demo})
                continue()
            endif()
            set(_preset_src ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/presets/${demo})
            set(_preset_dir ${CMAKE_CURRENT_BINARY_DIR}/presets/${demo})
            file(COPY ${_preset_src}/ DESTINATION ${_preset_dir} FILES_MATCHING PATTERN "*.json")
            if(NOT TARGET ${demo}_vstpreset_exporter) # A demo with its own presets made them already.
                make_vstpreset_exporter(${demo}_lib ${_preset_dir})
            endif()
            if(NOT TARGET ${demo}_tfx_exporter)
                make_tfx_exporter(${demo}_lib ${_preset_dir})
            endif()
            add_dependencies(tiny_host_tests ${demo}_vstpreset_exporter ${demo}_tfx_exporter)

            add_test(NAME export.${demo}.vst3 COMMAND ${demo}_vstpreset_exporter ${_preset_dir})
            add_test(NAME export.${demo}.aax COMMAND ${demo}_tfx_exporter ${_preset_dir})
            set_tests_properties(export.${demo}.vst3 export.${demo}.aax PROPERTIES LABELS host FIXTURES_SETUP exported_${demo})

            if(WIN32)
                set(_vst3_path $<TARGET_FILE:${demo}_vst3>)
            else()
                set(_vst3_path $<TARGET_BUNDLE_DIR:${demo}_vst3>)
            endif()
            add_test(NAME vst3_preset.${demo} COMMAND vst3_host ${_vst3_path} --fixtures ${_preset_src} --preset "${_preset_dir}/out/Round Trip.vstpreset")
            add_test(NAME aax_preset.${demo} COMMAND aax_host_${demo} --fixtures ${_preset_src} --preset "${_preset_dir}/out/Round Trip.tfx")
            set_tests_properties(vst3_preset.${demo} aax_preset.${demo} PROPERTIES TIMEOUT 60 LABELS host FIXTURES_REQUIRED exported_${demo})
        endforeach()
    endif()
endif()

# The suppressions reach every test here however ctest is run; the presets pass the rest of TSAN_OPTIONS.
if(TINY_SANITIZE STREQUAL "thread")
    get_property(_tests DIRECTORY PROPERTY TESTS)
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION "TSAN_OPTIONS=string_append::suppressions=${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/tsan.supp")
endif()
