add_library(drawcore STATIC
    Annotations.cpp
    Annotations.h
    Builders.cpp
    Builders.h
    Dimensions.cpp
    Expression.cpp
    Parameters.cpp
    Parameters.h
    Dimensions.h
    Export.cpp
    Export.h
    Features.cpp
    Features.h
    Curves.cpp
    Geometry.cpp
    Geometry.h
    Gradient.cpp
    Hatch.cpp
    Measure.cpp
    Measure.h
    Gradient.h
    Groups.cpp
    Groups.h
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
setsquare_warnings(drawcore)
