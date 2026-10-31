add_library(drawcore STATIC
    Dimensions.cpp
    Dimensions.h
    Geometry.cpp
    Geometry.h
    Serialize.cpp
    Serialize.h
    Sketch.cpp
    Sketch.h
    Solver.cpp
    Solver.h
)

target_include_directories(drawcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)  # #include <core/Sketch.h>
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(drawcore PRIVATE planegcs nlohmann_json::nlohmann_json)
target_compile_features(drawcore PUBLIC cxx_std_20)
set_target_properties(drawcore PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
drawapp_warnings(drawcore)
