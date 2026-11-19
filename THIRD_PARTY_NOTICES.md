# Third-party software

Setsquare is free software under the GNU General Public License, version 3 or (at your option)
any later version (see `LICENSE`). It's built on, and its Windows download includes, the
following, each under its own licence. In the download, their licence texts are in `licenses/`.

| What | Used for | Licence |
|---|---|---|
| [PlaneGCS](https://github.com/FreeCAD/FreeCAD) (from FreeCAD's Sketcher; vendored in `third_party/planegcs`, with local changes listed in its README) | the constraint solver | LGPL-2.1-or-later |
| [Qt 6](https://www.qt.io) (© The Qt Company Ltd and other contributors) | the user interface, drawing, SVG, PDF | LGPL-3.0 |
| [Python](https://www.python.org) (CPython, with its standard library) | scripting | PSF License |
| [pybind11](https://github.com/pybind/pybind11) | connecting Python to the app | BSD-3-Clause |
| [Eigen](https://eigen.tuxfamily.org) | the solver's linear algebra | MPL-2.0 |
| [Boost](https://www.boost.org) (Graph and the libraries it uses) | the solver | BSL-1.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | the file format | MIT |
| zlib, bzip2, xz (liblzma), Zstandard, Expat, libffi, SQLite, mpdecimal, OpenSSL | Python's standard library | their own (see `licenses/`) |
| Microsoft Visual C++ runtime (Windows download only) | running the program | Microsoft's redistributable licence |

Catch2 (BSL-1.0) is used by the tests only and isn't in the download.

Qt is linked dynamically: the download has Qt's own DLLs, unmodified, which can be swapped for
other builds of the same Qt version. Qt's source is at https://download.qt.io/.