# ============================================================================
# tinyplug — the model-aware half of the framework. Header-only on purpose: it has
# no translation units of its own, so its headers only ever compile inside a plug-in
# target, where the generated <tiny_models.hpp> / <tiny_plugin.hpp> are on the path.
# A compiled-once library here would see no models and silently disagree with the
# plug-in about the layout of every model-dependent type.
# ============================================================================

set(TINY_PLUG_LIB tinyplug)
add_library(${TINY_PLUG_LIB} INTERFACE)

target_sources(${TINY_PLUG_LIB} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include FILES
    include/tinyplug/action_queue.hpp
    include/tinyplug/tiny_edit.hpp
    include/tinyplug/tiny_events.hpp
    include/tinyplug/tiny_note_io.hpp
    include/tinyplug/tiny_processor.hpp
    include/tinyplug/tiny_state_link.hpp
    include/tinyplug/tiny_view.hpp
    include/tinyplug/tiny_worker.hpp
    include/tinyplug/tinyplug.hpp
    include/tinyplug/undo_history.hpp
)

# Public include root → consumers write <tinyplug/...>.
target_include_directories(${TINY_PLUG_LIB} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${TINY_PLUG_LIB} INTERFACE ${TINY_CORE_LIB})

# Forward to parent scope.
set(TINY_PLUG_LIB ${TINY_PLUG_LIB} PARENT_SCOPE)
