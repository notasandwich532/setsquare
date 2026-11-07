find_package(Catch2 3 CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test Svg)
include(Catch)

# Where Qt's DLLs live, so tests can run on Windows without deploying Qt next to them.
set(QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

# --- Core: model, solver, geometry (no Qt) ---
add_executable(core_tests
    core/test_builders.cpp
    core/test_dimensions.cpp
    core/test_expression.cpp
    core/test_export.cpp
    core/test_features.cpp
    core/test_geometry.cpp
    core/test_modify.cpp
    core/test_planegcs.cpp
    core/test_regions.cpp
    core/test_serialize.cpp
    core/test_sketch.cpp
    core/test_solver.cpp
    core/test_style.cpp
    core/test_text.cpp
)
target_link_libraries(core_tests PRIVATE drawcore planegcs Catch2::Catch2WithMain)
set_target_properties(core_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
drawapp_warnings(core_tests)
catch_discover_tests(core_tests)

# --- App: drives the real view and tools with simulated input, headless ---
add_executable(app_tests
    app/main.cpp
    app/test_constraint_tools.cpp
    app/test_document.cpp
    app/test_export.cpp
    app/test_gradient_tool.cpp
    app/test_icons.cpp
    app/test_images.cpp
    app/test_mainwindow.cpp
    app/test_modify_tools.cpp
    app/test_page.cpp
    app/test_scripting.cpp
    app/test_shape_tools.cpp
    app/test_style_tools.cpp
    app/test_sublayers.cpp
    app/test_text.cpp
    app/test_tools.cpp
    app/test_transform_tools.cpp
)
target_link_libraries(app_tests PRIVATE drawapp_lib Qt6::Test Qt6::Svg Catch2::Catch2)
drawapp_warnings(app_tests)
drawapp_embeds_python(app_tests)
add_dependencies(app_tests python_runtime)
catch_discover_tests(app_tests
    DL_PATHS "${QT_BIN_DIR}"
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# --- Python scenario scripts, run headless: each tests/scripts/test_*.py is one test ---
file(GLOB script_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_*.py")
foreach(script IN LISTS script_tests)
    get_filename_component(name "${script}" NAME_WE)
    add_test(NAME "script: ${name}" COMMAND drawapp-cli "${script}")
    set_tests_properties("script: ${name}" PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR}"
    )
endforeach()
