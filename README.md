# Setsquare

A personal 2D parametric drawing app: Fusion-style constrained sketching, built on FreeCAD's PlaneGCS solver, with colours and other features layered on top. C++20 / Qt 6, dark mode by default. Made for Windows; it can be compiled for Linux too (see below).

## Prerequisites
- **Windows:** Visual Studio 2022 Build Tools (Desktop development with C++), Qt 6.8 (MSVC 2022 64-bit), vcpkg
- **Linux** (possible, but untested): GCC 13+, CMake 3.25+, Ninja, Qt 6.8, vcpkg

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
Linux: use the `linux-gcc` preset instead. It's possible to compile Setsquare there, but it isn't built or tested on Linux, so it may need small fixes (such as a missing `#include` that MSVC happens to bring in by itself).

The first configure builds Eigen, Boost.Graph and Catch2 through vcpkg, which takes a few minutes.

On Windows, the build copies the Qt DLLs next to `setsquare.exe` (windeployqt), so it starts on a double-click.

## Sharing it (Windows)
```
cmake --build --preset windows-msvc --target package_app
```
makes `build\windows-msvc\package\Setsquare.zip` (about 45 MB): the app, `setsquare-cli`, Qt, the bundled Python and Microsoft's C++ runtime, with a READ ME, so it runs on a PC with nothing installed (64-bit Windows 10 or 11). Unzip and double-click `setsquare.exe`. It isn't code-signed, so Windows SmartScreen asks once ("More info" → "Run anyway"). See `tools/package.cmake`.

## Scripting
Everything in the UI can be done from Python (bundled CPython 3.14, no system Python needed), and UI actions are recorded as Python in the console. You can run Python in three places:
- the in-app **Python** dock
- **Macros → Run Script…**
- headless, with `setsquare-cli script.py`

See [docs/scripting.md](docs/scripting.md).

## Layout
- `third_party/planegcs/`: vendored solver (see its README for the local patches)
- `src/core/`: sketch model, solver adapter, geometry, file format (no Qt)
- `src/app/`: `Document` + the recording `Editor` (the command layer), and the Qt application
- `src/script/`: embedded Python host, the `setsquare` module, and `setsquare-cli`
- `tests/`: Catch2 unit/GUI tests, plus `tests/scripts/*.py` run headless

## Credits
Setsquare was developed with [Claude](https://www.anthropic.com/claude), Anthropic's AI model, working in [Claude Code](https://www.anthropic.com/claude-code). The design, the decisions and the testing in use are the author's; Claude wrote all of the code, to that direction.

The constraint solver is PlaneGCS, from [FreeCAD](https://www.freecad.org)'s Sketcher.

## Licence
Setsquare is free software under the [GNU General Public License v3.0 or later](LICENSE). It's built on PlaneGCS (from FreeCAD, LGPL-2.1-or-later), Qt (LGPL-3.0), Python and other libraries: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The Windows zip includes all of their licence texts.
