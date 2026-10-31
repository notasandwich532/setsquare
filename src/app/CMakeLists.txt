qt_add_executable(drawapp
    main.cpp
    MainWindow.cpp
    MainWindow.h
    SketchView.cpp
    SketchView.h
    Theme.cpp
    Theme.h
)

target_link_libraries(drawapp PRIVATE Qt6::Widgets)
drawapp_warnings(drawapp)

set_target_properties(drawapp PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
