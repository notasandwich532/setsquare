# Use vcpkg's CPython only. Without these hints CMake happily finds whatever Python is
# installed on the machine (registry, PATH, Windows Store), which is exactly what bundling
# is meant to avoid.
set(_py_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
set(Python_ROOT_DIR "${_py_root}")
set(Python_FIND_STRATEGY LOCATION)
set(Python_FIND_REGISTRY NEVER)
set(Python_FIND_FRAMEWORK NEVER)
set(Python_FIND_VIRTUALENV STANDARD)
if(WIN32)
    set(Python_EXECUTABLE "${_py_root}/tools/python3/python.exe")
else()
    file(GLOB _py_exes "${_py_root}/tools/python3/python3*")
    if(_py_exes)
        list(GET _py_exes 0 Python_EXECUTABLE)
    endif()
endif()
find_package(Python 3.14 EXACT REQUIRED COMPONENTS Interpreter Development.Embed)
if(NOT Python_LIBRARY_RELEASE MATCHES "vcpkg_installed" AND NOT Python_LIBRARIES MATCHES "vcpkg_installed")
    message(FATAL_ERROR "Found a Python that isn't vcpkg's: ${Python_LIBRARIES}")
endif()
message(STATUS "Bundled Python ${Python_VERSION}: ${Python_LIBRARIES}")
set(PYBIND11_FINDPYTHON ON)  # pybind11: use the Python found above
find_package(pybind11 CONFIG REQUIRED)

# The embedded interpreter and the `setsquare` Python module.
add_library(drawscript STATIC
    PythonHost.cpp
    PythonHost.h
)
target_include_directories(drawscript PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drawscript PUBLIC drawdoc PRIVATE pybind11::embed)
setsquare_warnings(drawscript)
if(MSVC)
    target_compile_options(drawscript PRIVATE /bigobj)  # (the Python bindings are one big file)
endif()

# Headless script runner (console program; an offscreen GUI app, for fonts): setsquare-cli script.py [args...]
add_executable(setsquare-cli cli.cpp)
target_link_libraries(setsquare-cli PRIVATE drawscript Qt6::Core Qt6::Gui)
setsquare_warnings(setsquare-cli)
setsquare_embeds_python(setsquare-cli)
add_dependencies(setsquare-cli python_runtime)

# --- Bundled Python standard library ----------------------------------------------------------
# vcpkg builds CPython for us; its standard library is copied next to the executables as
# python/, which PythonHost uses as PYTHONHOME. No system Python is involved at runtime.
set(_py_out "${CMAKE_BINARY_DIR}/python")
set(_py_excludes
    PATTERN "__pycache__" EXCLUDE
    PATTERN "test" EXCLUDE
    PATTERN "tests" EXCLUDE
    PATTERN "idlelib" EXCLUDE
    PATTERN "tkinter" EXCLUDE
    PATTERN "turtledemo" EXCLUDE
    PATTERN "ensurepip" EXCLUDE
    PATTERN "site-packages" EXCLUDE
)
if(WIN32)
    # Windows layout: <home>/Lib (pure Python) and <home>/DLLs (extension modules).
    set(_py_lib "${_py_root}/tools/python3/Lib")
    set(_py_dlls "${_py_root}/tools/python3/DLLs")
    if(NOT EXISTS "${_py_lib}")
        message(FATAL_ERROR "Python standard library not found at ${_py_lib}")
    endif()
    file(COPY "${_py_lib}/" DESTINATION "${_py_out}/Lib" ${_py_excludes})
    if(EXISTS "${_py_dlls}")
        file(COPY "${_py_dlls}/" DESTINATION "${_py_out}/DLLs")
    endif()
else()
    # POSIX layout: <home>/lib/pythonX.Y
    file(GLOB _py_libdirs LIST_DIRECTORIES true "${_py_root}/lib/python3.*")
    list(FILTER _py_libdirs EXCLUDE REGEX "\\.(a|so)")
    list(GET _py_libdirs 0 _py_lib)
    get_filename_component(_py_ver "${_py_lib}" NAME)
    file(COPY "${_py_lib}/" DESTINATION "${_py_out}/lib/${_py_ver}" ${_py_excludes})
endif()
add_custom_target(python_runtime)  # the copy happens at configure time; targets depend on this for clarity
