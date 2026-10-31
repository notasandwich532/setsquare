find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(core_tests
    core/test_planegcs.cpp
    core/test_sketch.cpp
    core/test_solver.cpp
)
target_link_libraries(core_tests PRIVATE drawcore planegcs Catch2::Catch2WithMain)
set_target_properties(core_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
drawapp_warnings(core_tests)

catch_discover_tests(core_tests)
