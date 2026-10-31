find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(core_tests
    core/test_planegcs.cpp
)
target_link_libraries(core_tests PRIVATE planegcs Catch2::Catch2WithMain)
drawapp_warnings(core_tests)

catch_discover_tests(core_tests)
