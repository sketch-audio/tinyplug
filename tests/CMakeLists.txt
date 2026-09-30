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
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120) # A hang fails instead of stalling the run.
    add_dependencies(tiny_tests ${name})
endfunction()

set(_standin ${CMAKE_CURRENT_SOURCE_DIR}/support/generated)      # <tiny_models.hpp> with no models.
set(_work_standin ${CMAKE_CURRENT_SOURCE_DIR}/support/work_models) # <tiny_models.hpp> with a work model.

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

# Meters, blocks, notes.
tiny_add_test(tiny_meters_test)
tiny_add_test(tiny_blocks_test)
tiny_add_test(notes_test)

# State.
tiny_add_test(state_store_test)
tiny_add_test(state_sequencer_test)
tiny_add_test(state_stress_test)
tiny_add_test(state_retry_test)
tiny_add_test(state_writers_test)
tiny_add_test(state_record_test)
tiny_add_test(state_adapter_test)
tiny_add_test(state_link_test LIBS ${TINY_PLUG_LIB} INCLUDES ${_standin})

# ============================================================================
# Fake hosts: real plug-in bundles driven through each format's host API, one CTest per demo.
# Only when the plug-ins are built (the `tsan-hosts` / `asan-hosts` presets).
# ============================================================================

if(TINY_BUILD_PLUGINS)
    add_custom_target(tiny_host_tests)
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
            add_test(NAME aax_host.${demo} COMMAND aax_host_${demo})
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
            add_test(NAME vst3_host.${demo} COMMAND vst3_host ${_vst3_path})
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
            add_test(NAME auv2_host.${demo} COMMAND auv2_host $<TARGET_BUNDLE_DIR:${demo}_auv2>)
            set_tests_properties(auv2_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_auv2)
        endif()
        if(TARGET auv3_host AND TARGET ${demo}_auv3_test AND TARGET ${demo}_auv2)
            add_test(NAME auv3_host.${demo} COMMAND auv3_host $<TARGET_BUNDLE_DIR:${demo}_auv3_test> $<TARGET_BUNDLE_DIR:${demo}_auv2>)
            set_tests_properties(auv3_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_auv3_test)
        endif()
        if(TARGET ${demo}_clap)
            if(WIN32) # Not a CMake bundle there; the host finds the bundle from the binary.
                add_test(NAME clap_host.${demo} COMMAND clap_host $<TARGET_FILE:${demo}_clap>)
            else()
                add_test(NAME clap_host.${demo} COMMAND clap_host $<TARGET_BUNDLE_DIR:${demo}_clap>)
            endif()
            set_tests_properties(clap_host.${demo} PROPERTIES TIMEOUT 120 LABELS host)
            add_dependencies(tiny_host_tests ${demo}_clap)
        endif()
    endforeach()
endif()
