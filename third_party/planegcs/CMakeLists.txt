find_package(Eigen3 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS graph math)

add_library(planegcs STATIC
    planegcs/Constraints.cpp
    planegcs/GCS.cpp
    planegcs/Geo.cpp
    planegcs/qp_eq.cpp
    planegcs/SubSystem.cpp
)

target_include_directories(planegcs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}        # consumers use #include <planegcs/GCS.h>
        ${CMAKE_CURRENT_SOURCE_DIR}/shim   # SketcherGlobal.h is included from the public headers
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/planegcs
)

target_link_libraries(planegcs PUBLIC Eigen3::Eigen Boost::graph Boost::math)
target_compile_features(planegcs PUBLIC cxx_std_20)
# Upstream sources use C++23 library bits (std::unreachable); only the solver's own TUs need it.
set_target_properties(planegcs PROPERTIES CXX_STANDARD 23 AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# Upstream requires EIGEN_NO_DEBUG (SparseQR assertion workaround).
target_compile_definitions(planegcs PUBLIC EIGEN_NO_DEBUG)

# Third-party code: keep it quiet so our own warnings stay readable.
if(MSVC)
    target_compile_options(planegcs PRIVATE /W0 /bigobj)
else()
    target_compile_options(planegcs PRIVATE -w)
endif()
