# The document model and the recording Editor (Qt Core only). Shared by the UI and the
# Python host, which is why it's separate from the widgets.
add_library(drawdoc STATIC
    Document.cpp
    Document.h
    Editor.cpp
    Editor.h
)
target_include_directories(drawdoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drawdoc PUBLIC drawcore Qt6::Core)
drawapp_warnings(drawdoc)

# Everything else except main() lives in a library so GUI tests can drive the real widgets.
add_library(drawapp_lib STATIC
    ConstraintPanel.cpp
    ConstraintPanel.h
    FrameClock.cpp
    FrameClock.h
    MainWindow.cpp
    MainWindow.h
    ScriptConsole.cpp
    ScriptConsole.h
    SketchView.cpp
    SketchView.h
    SketchViewAnnotations.cpp
    Theme.cpp
    Theme.h
    tools/ConstraintTools.cpp
    tools/ConstraintTools.h
    tools/DrawTools.cpp
    tools/DrawTools.h
    tools/SelectTool.cpp
    tools/SelectTool.h
    tools/Tool.h
)
target_include_directories(drawapp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drawapp_lib PUBLIC drawdoc drawscript Qt6::Widgets)
if(WIN32)
    target_link_libraries(drawapp_lib PRIVATE dxgi dwmapi)  # vblank pacing (FrameClock)
endif()
drawapp_warnings(drawapp_lib)

qt_add_executable(drawapp main.cpp)
target_link_libraries(drawapp PRIVATE drawapp_lib)
drawapp_warnings(drawapp)
drawapp_embeds_python(drawapp)
add_dependencies(drawapp python_runtime)

set_target_properties(drawapp PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
