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
target_link_libraries(drawdoc PUBLIC drawcore Qt6::Core Qt6::Gui)
drawapp_warnings(drawdoc)

# Everything else except main() lives in a library so GUI tests can drive the real widgets.
add_library(drawapp_lib STATIC
    ColorBar.cpp
    ColorBar.h
    GradientEditor.cpp
    GradientEditor.h
    ImportDialog.cpp
    ImportDialog.h
    PageSetup.cpp
    PageSetup.h
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
    tools/GradientTool.h
    tools/ImageTool.cpp
    tools/ImageTool.h
    tools/Tool.h
    tools/Handles.cpp
    tools/Handles.h
    tools/TextTool.cpp
    tools/TextTool.h
    tools/TransformTools.cpp
    tools/TransformTools.h
)
target_include_directories(drawapp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drawapp_lib PUBLIC drawdoc drawscript Qt6::Widgets Qt6::Svg)

# Tool icons (made by icons/build_icons.py), compiled in as :/icons/<name>.svg.
file(GLOB tool_icons CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/icons/*.svg")
qt_add_resources(drawapp_lib "tool_icons" PREFIX "/icons" BASE "${PROJECT_SOURCE_DIR}/icons" FILES ${tool_icons})
# The app's own icon (drawn with the app: drawings/app_icon.dsk), as :/icons/app/drawapp.svg.
qt_add_resources(drawapp_lib "app_icon" PREFIX "/icons" BASE "${PROJECT_SOURCE_DIR}/icons" FILES "${PROJECT_SOURCE_DIR}/icons/app/drawapp.svg")
if(WIN32)
    target_link_libraries(drawapp_lib PRIVATE dxgi dwmapi)  # vblank pacing (FrameClock)
endif()
drawapp_warnings(drawapp_lib)

qt_add_executable(drawapp main.cpp)
if(WIN32)
    target_sources(drawapp PRIVATE drawapp.rc)  # the .exe's icon (icons/app/drawapp.ico)
endif()
target_link_libraries(drawapp PRIVATE drawapp_lib)
drawapp_warnings(drawapp)
drawapp_embeds_python(drawapp)
add_dependencies(drawapp python_runtime)

set_target_properties(drawapp PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Remakes icons/app/drawapp.ico from icons/app/drawapp.svg (not built by default):
#   cmake --build --preset windows-msvc --target make_app_icon
#   make_app_icon icons/app/drawapp.svg icons/app/drawapp.ico [preview.png]
add_executable(make_app_icon EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/tools/make_app_icon.cpp")
target_link_libraries(make_app_icon PRIVATE Qt6::Gui Qt6::Svg)
drawapp_warnings(make_app_icon)