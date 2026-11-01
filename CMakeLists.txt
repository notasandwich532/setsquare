cmake_minimum_required(VERSION 3.25)

project(drawing_app VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DRAWAPP_BUILD_TESTS "Build unit and script tests" ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
endif()

# Warning flags for our own targets (third-party code opts out).
function(drawapp_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Executables that embed Python: on Linux they must export the interpreter's symbols so the
# standard library's extension modules (.so) can load.
function(drawapp_embeds_python target)
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    endif()
endfunction()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup()

add_subdirectory(third_party/planegcs)
add_subdirectory(src)

if(DRAWAPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
