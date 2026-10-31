add_library(drawcore STATIC
    Geometry.cpp
    Geometry.h
    Sketch.cpp
    Sketch.h
    Solver.cpp
    Solver.h
)

target_include_directories(drawcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)  # #include <core/Sketch.h>
target_link_libraries(drawcore PRIVATE planegcs)
target_compile_features(drawcore PUBLIC cxx_std_20)
set_target_properties(drawcore PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
drawapp_warnings(drawcore)
