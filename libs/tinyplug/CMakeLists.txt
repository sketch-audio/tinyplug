include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.zip
    URL_HASH SHA256=34660b5e9a407195d55e8da705ed26cc6d175ce5a6b1fb957e701fb4d5b04022
)
FetchContent_MakeAvailable(nlohmann_json)

# ============================================================================
# Core library — format-agnostic framework (params, events, state, tasks, view
# loop). Pure C++: no Skia, no native frameworks. Only forward-declares SkCanvas.
# ============================================================================

set(TINY_SHARED_LIB tiny_shared_lib)
add_library(${TINY_SHARED_LIB} STATIC)

target_sources(${TINY_SHARED_LIB} PRIVATE
    source/action_queue.cpp
    source/gesture_recognizers.cpp
    source/notification_queue.cpp
    source/serial_queue.cpp
    source/state_adapter.cpp
    source/task_launcher.cpp
    source/task_manager.cpp
    source/undo_history.cpp
    source/host_formatter.cpp
    source/tiny_log.cpp
    source/value_helper.cpp
)

target_sources(${TINY_SHARED_LIB} PRIVATE
    include/tinyplug/platform_defs.hpp
    include/tinyplug/lifecycle_probe.hpp
    include/tinyplug/tiny_events.hpp
    include/tinyplug/tiny_meters.hpp
    include/tinyplug/tiny_params.hpp
    include/tinyplug/value_helper.hpp
    include/tinyplug/tiny_processor.hpp
    include/tinyplug/lock_free_queue.hpp
    include/tinyplug/tiny_log.hpp
    include/tinyplug/tiny_utils.hpp
    include/tinyplug/tiny_view.hpp
    include/tinyplug/tiny_worker.hpp
    include/tinyplug/tinyplug.hpp
)

# Public include root → consumers write <tinyplug/...>.
target_include_directories(${TINY_SHARED_LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${TINY_SHARED_LIB} PUBLIC nlohmann_json::nlohmann_json)

# Logging / lifecycle probing. PUBLIC: the wrappers link this lib and their probe call
# sites have to agree with the library about whether the layer exists at all.
target_compile_definitions(${TINY_SHARED_LIB} PUBLIC TINY_LOG_ENABLED=$<BOOL:${TINY_LOG}>)

# Forward to parent scope.
set(TINY_SHARED_LIB ${TINY_SHARED_LIB} PARENT_SCOPE)
