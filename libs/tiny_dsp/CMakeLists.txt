# ============================================================================
# tiny_dsp — header-only DSP helpers (Host_bypass, Linear_ramper, Delay_line).
# Pure leaf: depends on nothing but the standard library. Consumers link this
# target to gain <tiny_dsp/...> on their include path; isolated from core.
# ============================================================================

set(TINY_DSP_LIB tiny_dsp)
add_library(${TINY_DSP_LIB} INTERFACE)

target_include_directories(${TINY_DSP_LIB} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(TINY_DSP_LIB ${TINY_DSP_LIB} PARENT_SCOPE)
