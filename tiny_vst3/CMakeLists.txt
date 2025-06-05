# VST3 target.
set(vst3_target ${tiny_base_filename}_vst3)

# The VST3 SDK apparently needs these to be defined.
if (CMAKE_BUILD_TYPE MATCHES Release)
    add_compile_definitions(NDEBUG RELEASE)
else()
    add_compile_definitions(_DEBUG DEVELOPMENT)
endif()

include(cmake/vst3_deps.cmake)
smtg_enable_vst3_sdk() # sdk

add_library(${vst3_target} MODULE
    source/adapters.h
    source/controller.h
    source/controller.cpp
    source/entry.cpp
    source/processor.h
    source/processor.cpp
    source/view.h
)

target_sources(${vst3_target} PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp)

target_link_libraries(${vst3_target} PRIVATE sdk)
target_link_libraries(${vst3_target} PRIVATE ${tiny_shared_lib})
target_link_libraries(${vst3_target} PRIVATE "-framework Foundation")

target_compile_options(${vst3_target} PRIVATE -Wall -Wextra -pedantic)


# Packaging.
set_target_properties(${vst3_target} PROPERTIES
    BUNDLE True
    OUTPUT_NAME ${tiny_base_filename}
    BUNDLE_EXTENSION vst3
    MACOSX_BUNDLE_GUI_IDENTIFIER ${tiny_base_identifier}.vst3
    MACOSX_BUNDLE_BUNDLE_NAME ${tiny_product_name}
    MACOSX_BUNDLE_BUNDLE_VERSION ${tiny_build_number}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${tiny_version_string}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
)

set(output_dir ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})

# Add the PkgInfo file. (Makes it so we appear as a bundle and not a folder in Finder.)
add_custom_command(
    TARGET ${vst3_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgInfo ${output_dir}/${tiny_base_filename}.vst3/Contents
    VERBATIM
)

# Copy the VST3 bundle to ~/Library/Audio/Plug-Ins/VST3/ after building.
add_custom_command(
    TARGET ${vst3_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $ENV{HOME}/Library/Audio/Plug-Ins/VST3/
    COMMAND ${CMAKE_COMMAND} -E copy_directory 
        ${output_dir}/${tiny_base_filename}.vst3 
        $ENV{HOME}/Library/Audio/Plug-Ins/VST3/${tiny_base_filename}.vst3
    VERBATIM
)