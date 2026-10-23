# ============================================================================
# tiny_text — editor text: embedded fonts (static or variable), HarfBuzz shaping and
# layout through SkParagraph, the platform's rasteriser. Model-free, compiled once.
# Skia is PRIVATE: the header names only SkCanvas, forward-declared; the static-lib
# link still carries Skia to the final module, as tiny_platform's does.
# ============================================================================

set(TINY_TEXT_LIB tiny_text)
add_library(${TINY_TEXT_LIB} STATIC)

target_sources(${TINY_TEXT_LIB} PRIVATE
    include/tiny_text/fonts.hpp
    source/fonts.cpp
)

target_include_directories(${TINY_TEXT_LIB} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${TINY_TEXT_LIB} PUBLIC ${TINY_CORE_LIB})
target_link_libraries(${TINY_TEXT_LIB} PRIVATE tiny::skparagraph tiny::skia)
if(APPLE)
    target_link_libraries(${TINY_TEXT_LIB} PUBLIC "-framework CoreText" "-framework CoreFoundation" "-framework CoreGraphics")
endif()

set(TINY_TEXT_LIB ${TINY_TEXT_LIB} PARENT_SCOPE)
