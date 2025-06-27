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
    source/vst3_controller.cpp
    source/vst3_entry.cpp
    source/vst3_processor.cpp
)

if(APPLE)
    target_sources(${vst3_target} PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp)
elseif(WIN32)
    target_sources(${vst3_target} PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp)
endif()

target_link_libraries(${vst3_target} PRIVATE sdk)
target_link_libraries(${vst3_target} PRIVATE ${tiny_shared_lib})

if(APPLE)
    target_link_libraries(${vst3_target} PRIVATE "-framework Cocoa")
    target_compile_options(${vst3_target} PRIVATE -Wall -Wextra -pedantic)
elseif(WIN32)
    target_compile_options(${vst3_target} PRIVATE /W4)
endif()


# Packaging.
if(APPLE)
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
elseif(WIN32)
    set_target_properties(${vst3_target} PROPERTIES
        OUTPUT_NAME ${tiny_base_filename}
        SUFFIX .vst3
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.vst3/Contents/x86_64-win
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.vst3/Contents/x86_64-win
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_BINARY_DIR}/${tiny_base_filename}.vst3/Contents/x86_64-win
    )

    # Right now have to manually copy to destination directory.
endif()