# Everything except main() lives in a library so GUI tests can drive the real widgets.
add_library(drawapp_lib STATIC
    Document.cpp
    Document.h
    FrameClock.cpp
    FrameClock.h
    MainWindow.cpp
    MainWindow.h
    SketchView.cpp
    SketchView.h
    Theme.cpp
    Theme.h
    tools/DrawTools.cpp
    tools/DrawTools.h
    tools/SelectTool.cpp
    tools/SelectTool.h
    tools/Tool.h
)
target_include_directories(drawapp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drawapp_lib PUBLIC drawcore Qt6::Widgets)
if(WIN32)
    target_link_libraries(drawapp_lib PRIVATE dxgi dwmapi)  # vblank pacing (FrameClock)
endif()
drawapp_warnings(drawapp_lib)

qt_add_executable(drawapp main.cpp)
target_link_libraries(drawapp PRIVATE drawapp_lib)
drawapp_warnings(drawapp)

set_target_properties(drawapp PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
