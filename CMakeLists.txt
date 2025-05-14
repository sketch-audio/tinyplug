cmake_minimum_required(VERSION 3.25) # ???

project(tinyplug)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64") # !!!
endif()

add_subdirectory(tiny_shared)
add_subdirectory(tiny_vst3)