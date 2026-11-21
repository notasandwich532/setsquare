# The document model and the recording Editor, and text shapes from fonts (Qt Core and Gui, no
# widgets). Shared by the UI and the Python host, which is why it's separate from the widgets.
add_library(drawdoc STATIC
    Document.cpp
    Document.h
    Editor.cpp
    Editor.h
    Images.cpp
    Images.h
    Import.cpp
    Import.h
    TextShapes.cpp
    TextShapes.h
)
target_include_directories(drawdoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drawdoc PUBLIC drawcore Qt6::Core Qt6::Gui Qt6::Svg)  # (Svg: the PDF export draws the SVG export)
setsquare_warnings(drawdoc)

# Everything else except main() lives in a library so GUI tests can drive the real widgets.
add_library(setsquare_lib STATIC
    ColorBar.cpp
    ColorBar.h
    GradientEditor.cpp
    GradientEditor.h
    ImportDialog.cpp
    ImportDialog.h
    PageSetup.cpp
    PageSetup.h
    Recovery.cpp
    Recovery.h
    CommandPanel.cpp
    CommandPanel.h
    NumberInput.cpp
    NumberInput.h
    BrowserPanel.cpp
    ConstraintPanel.cpp
    FontPicker.cpp
    FontPicker.h
    ConstraintPanel.h
    FrameClock.cpp
    FrameClock.h
    Icons.cpp
    Icons.h
    MainWindow.cpp
    MainWindow.h
    ScriptConsole.cpp
    ScriptConsole.h
    SketchView.cpp
    SketchView.h
    SketchViewAnnotations.cpp
    ToolSearch.cpp
    ToolSearch.h
    Theme.cpp
    Theme.h
    TiledFill.cpp
    TiledFill.h
    tools/ConstraintTools.cpp
    tools/ConstraintTools.h
    tools/CurveTools.cpp
    tools/CurveTools.h
    tools/DrawTools.cpp
    tools/ModifyTools.cpp
    tools/ModifyTools.h
    tools/ProjectTool.cpp
    tools/ProjectTool.h
    tools/DrawTools.h
    tools/SelectTool.cpp
    tools/SelectTool.h
    tools/ShapeTools.cpp
    tools/ShapeTools.h
    tools/StyleTools.cpp
    tools/StyleTools.h
    tools/GradientTool.cpp
    tools/HatchTool.cpp
    tools/GradientTool.h
    tools/ImageTool.cpp
    tools/ImageTool.h
    tools/AnnotateTool.cpp
    tools/AnnotateTool.h
    tools/MeasureTool.cpp
    tools/MeasureTool.h
    tools/Tool.h
    tools/Handles.cpp
    tools/Handles.h
    tools/TextTool.cpp
    tools/TextTool.h
    tools/TransformTools.cpp
    tools/TransformTools.h
)
target_include_directories(setsquare_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(setsquare_lib PUBLIC drawdoc drawscript Qt6::Widgets Qt6::Svg)

# Tool icons (made by icons/build_icons.py), compiled in as :/icons/<name>.svg.
file(GLOB tool_icons CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/icons/*.svg")
qt_add_resources(setsquare_lib "tool_icons" PREFIX "/icons" BASE "${PROJECT_SOURCE_DIR}/icons" FILES ${tool_icons})
# The app's own icon (drawn with the app: drawings/app_icon.setsq), as :/icons/app/setsquare.svg.
qt_add_resources(setsquare_lib "app_icon" PREFIX "/icons" BASE "${PROJECT_SOURCE_DIR}/icons" FILES "${PROJECT_SOURCE_DIR}/icons/app/setsquare.svg")
if(WIN32)
    target_link_libraries(setsquare_lib PRIVATE dxgi dwmapi)  # vblank pacing (FrameClock)
endif()
setsquare_warnings(setsquare_lib)

qt_add_executable(setsquare main.cpp)
if(WIN32)
    target_sources(setsquare PRIVATE setsquare.rc)  # the .exe's icon (icons/app/setsquare.ico)
endif()
target_link_libraries(setsquare PRIVATE setsquare_lib)
setsquare_warnings(setsquare)
setsquare_embeds_python(setsquare)
add_dependencies(setsquare python_runtime)

set_target_properties(setsquare PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Copies Qt's DLLs and plugins next to setsquare.exe after each build, so it starts on a
# double-click. The test programs and setsquare-cli in the same folder then load Qt from there too,
# so the "offscreen" platform they run headless on is deployed as well.
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" "$ENV{QT_ROOT_DIR}/bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET setsquare POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime --verbose 0
                    --include-plugins qoffscreen "$<TARGET_FILE:setsquare>"
            COMMENT "Deploying Qt next to setsquare.exe"
            VERBATIM)
        # A copy ready to share, with everything it needs (see tools/package.cmake; not built by
        # default): build/<preset>/package/Setsquare/ and Setsquare.zip.
        #   cmake --build --preset windows-msvc --target package_app
        add_custom_target(package_app
            COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DOUT=${CMAKE_BINARY_DIR}/package"
                    "-DAPP=$<TARGET_FILE:setsquare>" "-DCLI=$<TARGET_FILE:setsquare-cli>"
                    "-DWINDEPLOYQT=${WINDEPLOYQT}" "-DVERSION=${PROJECT_VERSION}"
                    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DVCPKG_SHARE=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share"
                    -P "${PROJECT_SOURCE_DIR}/tools/package.cmake"
            DEPENDS setsquare setsquare-cli
            COMMENT "Packaging Setsquare to share"
            VERBATIM)
    endif()
endif()

# Remakes icons/app/setsquare.ico from icons/app/setsquare.svg (not built by default):
#   cmake --build --preset windows-msvc --target make_app_icon
#   make_app_icon icons/app/setsquare.svg icons/app/setsquare.ico [preview.png]
add_executable(make_app_icon EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/tools/make_app_icon.cpp")
target_link_libraries(make_app_icon PRIVATE Qt6::Gui Qt6::Svg)
setsquare_warnings(make_app_icon)