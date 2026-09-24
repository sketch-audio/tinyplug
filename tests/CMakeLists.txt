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
