find_package(Catch2 3 CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)
include(Catch)

# Where Qt's DLLs live, so tests can run on Windows without deploying Qt next to them.
set(QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

# --- Core: model, solver, geometry (no Qt) ---
add_executable(core_tests
    core/test_dimensions.cpp
    core/test_geometry.cpp
    core/test_planegcs.cpp
    core/test_sketch.cpp
    core/test_solver.cpp
)
target_link_libraries(core_tests PRIVATE drawcore planegcs Catch2::Catch2WithMain)
set_target_properties(core_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
drawapp_warnings(core_tests)
catch_discover_tests(core_tests)

# --- App: drives the real view and tools with simulated input, headless ---
add_executable(app_tests
    app/main.cpp
    app/test_constraint_tools.cpp
    app/test_tools.cpp
)
target_link_libraries(app_tests PRIVATE drawapp_lib Qt6::Test Catch2::Catch2)
drawapp_warnings(app_tests)
catch_discover_tests(app_tests
    DL_PATHS "${QT_BIN_DIR}"
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
