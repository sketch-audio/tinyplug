include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.zip
    URL_HASH SHA256=34660b5e9a407195d55e8da705ed26cc6d175ce5a6b1fb957e701fb4d5b04022
)
FetchContent_MakeAvailable(nlohmann_json)

# ============================================================================
# tiny_core — the model-free half of the framework, compiled once. Vocabulary
# (params, meters, input, notifications), value conversion, state, tasks, logging.
# Nothing here may name a user model type: see tinyplug for the model-aware half.
# ============================================================================

set(TINY_CORE_LIB tiny_core)
add_library(${TINY_CORE_LIB} STATIC)

target_sources(${TINY_CORE_LIB} PRIVATE
    source/gesture_recognizers.cpp
    source/host_formatter.cpp
    source/notification_queue.cpp
    source/relay.cpp
    source/serial_queue.cpp
    source/state_adapter.cpp
    source/task_launcher.cpp
    source/task_manager.cpp
    source/tiny_log.cpp
    source/value_helper.cpp
)

target_sources(${TINY_CORE_LIB} PRIVATE
    include/tiny_core/base64.hpp
    include/tiny_core/change_set.hpp
    include/tiny_core/data_port.hpp
    include/tiny_core/denormal_guard.hpp
    include/tiny_core/gesture_recognizers.hpp
    include/tiny_core/host_formatter.hpp
    include/tiny_core/lifecycle_probe.hpp
    include/tiny_core/lock_free_queue.hpp
    include/tiny_core/notification_queue.hpp
    include/tiny_core/platform_defs.hpp
    include/tiny_core/relay.hpp
    include/tiny_core/serial_queue.hpp
    include/tiny_core/state_adapter.hpp
    include/tiny_core/state_merge.hpp
    include/tiny_core/state_record.hpp
    include/tiny_core/state_rules.hpp
    include/tiny_core/state_store.hpp
    include/tiny_core/task_launcher.hpp
    include/tiny_core/task_manager.hpp
    include/tiny_core/tiny_blocks.hpp
    include/tiny_core/tiny_core.hpp
    include/tiny_core/tiny_input.hpp
    include/tiny_core/tiny_log.hpp
    include/tiny_core/tiny_meters.hpp
    include/tiny_core/tiny_notifications.hpp
    include/tiny_core/tiny_params.hpp
    include/tiny_core/tiny_state.hpp
    include/tiny_core/tiny_utils.hpp
    include/tiny_core/tiny_work.hpp
    include/tiny_core/value_helper.hpp
    include/tiny_core/window_token.hpp
)

# Public include root → consumers write <tiny_core/...>.
target_include_directories(${TINY_CORE_LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${TINY_CORE_LIB} PUBLIC nlohmann_json::nlohmann_json)

# Logging / lifecycle probing. PUBLIC: the wrappers link this lib and their probe call
# sites have to agree with the library about whether the layer exists at all.
target_compile_definitions(${TINY_CORE_LIB} PUBLIC TINY_LOG_ENABLED=$<BOOL:${TINY_LOG}>)

# Forward to parent scope.
set(TINY_CORE_LIB ${TINY_CORE_LIB} PARENT_SCOPE)
