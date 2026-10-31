# Drawing App

A personal 2D parametric drawing app: Fusion-style constrained sketching, built on FreeCAD's PlaneGCS solver, with colours and other features layered on top. C++20 / Qt 6, dark mode by default, Windows and Linux.

## Prerequisites
- **Windows:** Visual Studio 2022 Build Tools (Desktop development with C++), Qt 6.8 (MSVC 2022 64-bit), vcpkg
- **Linux:** GCC 13+, CMake 3.25+, Ninja, Qt 6.8, vcpkg

Environment variables:
- `VCPKG_ROOT`: your vcpkg checkout, e.g. `C:\dev\vcpkg`
- `QT_ROOT_DIR`: Qt kit directory, e.g. `C:\Qt\6.8.3\msvc2022_64`

## Build and test
Windows: run these from an **x64 Native Tools Command Prompt for VS 2022**, so `cl` and `ninja` are on PATH.
```
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```
Linux: use the `linux-gcc` preset instead.

The first configure builds Eigen, Boost.Graph and Catch2 through vcpkg, which takes a few minutes.

On Windows, `windeployqt build\windows-msvc\drawapp.exe` copies the Qt DLLs next to the exe. Alternatively, add `%QT_ROOT_DIR%\bin` to PATH.

## Layout
- `third_party/planegcs/`: vendored solver (see its README for the local patches)
- `src/app/`: Qt application (main window, sketch canvas, theme)
- `tests/`: Catch2 unit tests
