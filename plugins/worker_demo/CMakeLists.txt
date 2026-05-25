set(PLUGIN_TARGET WorkerDemo_lib) # Generated.

add_library(${PLUGIN_TARGET} STATIC)

# Your plug-in information goes here.
# ---
add_property(${PLUGIN_TARGET} TINY_COMPANY_NAME "tinyplug")
add_property(${PLUGIN_TARGET} TINY_COMPANY_WEBSITE "https://github.com/sketch-audio/tinyplug")
add_property(${PLUGIN_TARGET} TINY_COMPANY_EMAIL "info@tinyplug.com")

add_property(${PLUGIN_TARGET} TINY_FRAMEWORK_CODE "tiny")
add_property(${PLUGIN_TARGET} TINY_MANUFACTURER_CODE "Tiny")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_CODE "wrkr")

add_property(${PLUGIN_TARGET} TINY_PRODUCT_NAME "Worker Demo")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_NAME "Worker Demo")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_SHORT_NAME "Worker")

add_property(${PLUGIN_TARGET} TINY_BASE_FILENAME "WorkerDemo")
add_property(${PLUGIN_TARGET} TINY_BASE_IDENTIFIER "com.tinyplug.WorkerDemo")
add_property(${PLUGIN_TARGET} TINY_VERSION_STRING "0.1.0")
add_property(${PLUGIN_TARGET} TINY_BUILD_NUMBER "1")

add_property(${PLUGIN_TARGET} TINY_PLUGIN_WANTS_SIDECHAIN "false")
add_property(${PLUGIN_TARGET} TINY_PLUGIN_CAN_PROCESS_MONO "true")

add_property(${PLUGIN_TARGET} TINY_AAX_CATEGORIES "AAX_ePlugInCategory_None")

add_property(${PLUGIN_TARGET} TINY_CLAP_DESCRIPTION "Built with tinyplug.")
add_property(${PLUGIN_TARGET} TINY_CLAP_FEATURES "audio-effect;utility")

add_property(${PLUGIN_TARGET} TINY_AUV2_VIEW_CLASS "WorkerDemo_viewFactory")
add_property(${PLUGIN_TARGET} TINY_AUV2_TYPE "aufx")

add_property(${PLUGIN_TARGET} TINY_VST3_SUBCATEGORIES "Fx")
# ---

target_sources(${PLUGIN_TARGET} PRIVATE
    source/models/meter_model.h
    source/models/param_model.h
    source/plug_editor.cpp
    source/plug_editor.h
    source/plug_processor.cpp
    source/plug_processor.h
    source/plug_worker.h
)

configure_plug_info(
    ${PLUGIN_TARGET}
    ${CMAKE_CURRENT_BINARY_DIR}/plug_info.h
)

target_include_directories(${PLUGIN_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_SHARED_LIB})

make_aax_plugin(${PLUGIN_TARGET})
make_auv2_plugin(${PLUGIN_TARGET})
make_auv3_plugin(${PLUGIN_TARGET})
make_clap_plugin(${PLUGIN_TARGET})
make_vst3_plugin(${PLUGIN_TARGET})
