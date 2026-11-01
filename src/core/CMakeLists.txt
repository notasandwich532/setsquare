add_library(drawcore STATIC
    Builders.cpp
    Builders.h
    Dimensions.cpp
    Dimensions.h
    Features.cpp
    Features.h
    Geometry.cpp
    Geometry.h
    Modify.cpp
    Modify.h
    Regions.cpp
    Regions.h
    Serialize.cpp
    Serialize.h
    Sketch.cpp
    Sketch.h
    Solver.cpp
    Solver.h
    Style.cpp
    Style.h
    Transform.cpp
    Transform.h
)

target_include_directories(drawcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)  # #include <core/Sketch.h>
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(drawcore PRIVATE planegcs nlohmann_json::nlohmann_json)
target_compile_features(drawcore PUBLIC cxx_std_20)
set_target_properties(drawcore PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
drawapp_warnings(drawcore)
