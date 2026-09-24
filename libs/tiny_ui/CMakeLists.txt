# ============================================================================
# tiny_ui — header-only editor helpers: parameter controls, a theme, and Skia drawing.
# Header-only because controls.hpp includes the model-aware <tinyplug/tinyplug.hpp>.
# Skia stays the consumer's PRIVATE link; include draw.hpp from .cpp files only.
# ============================================================================

set(TINY_UI_LIB tiny_ui)
add_library(${TINY_UI_LIB} INTERFACE)

target_sources(${TINY_UI_LIB} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include FILES
    include/tiny_ui/controls.hpp
    include/tiny_ui/draw.hpp
)

target_include_directories(${TINY_UI_LIB} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${TINY_UI_LIB} INTERFACE ${TINY_PLUG_LIB})

set(TINY_UI_LIB ${TINY_UI_LIB} PARENT_SCOPE)
