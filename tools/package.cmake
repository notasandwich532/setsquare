# Makes a copy of the app ready to share (Windows): a folder with everything it needs to run on a
# PC with nothing installed, and that folder zipped. Run through the build:
#
#   cmake --build --preset windows-msvc --target package_app
#
# which gives build/windows-msvc/package/Setsquare/ and build/windows-msvc/package/Setsquare.zip.
#
# In it: setsquare.exe and setsquare-cli.exe; Qt (its DLLs and plugins, put there by windeployqt);
# the other DLLs the build uses (Python's, zlib, ...); the bundled Python (python/); Microsoft's C++
# runtime (so the Visual C++ Redistributable needn't be installed); a READ ME. Not the tests, the
# build's own files or debugging symbols.
#
# Expects: BUILD_DIR, OUT, APP, CLI, WINDEPLOYQT, VERSION; the C++ runtime is found from the
# Visual Studio environment (VCToolsRedistDir), else looked for where Visual Studio installs it.

cmake_minimum_required(VERSION 3.21)
foreach(var BUILD_DIR OUT APP CLI WINDEPLOYQT)
    if(NOT ${var})
        message(FATAL_ERROR "package.cmake: ${var} not set")
    endif()
endforeach()

set(dest "${OUT}/Setsquare")
file(REMOVE_RECURSE "${dest}" "${OUT}/Setsquare.zip")
file(MAKE_DIRECTORY "${dest}")

# The programs.
file(COPY "${APP}" "${CLI}" DESTINATION "${dest}")

# Qt: its DLLs and the plugins the app loads (the offscreen platform too, for setsquare-cli).
get_filename_component(app_name "${APP}" NAME)
get_filename_component(cli_name "${CLI}" NAME)
foreach(exe "${app_name}" "${cli_name}")
    execute_process(
        COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime --verbose 0
                --include-plugins qoffscreen "${dest}/${exe}"
        RESULT_VARIABLE deployed)
    if(NOT deployed EQUAL 0)
        message(FATAL_ERROR "windeployqt failed on ${exe}")
    endif()
endforeach()

# The build's other DLLs (Python's, zlib, ...): whatever's beside the programs that Qt's deploy
# didn't bring.
file(GLOB build_dlls "${BUILD_DIR}/*.dll")
foreach(dll IN LISTS build_dlls)
    get_filename_component(name "${dll}" NAME)
    if(NOT EXISTS "${dest}/${name}")
        file(COPY "${dll}" DESTINATION "${dest}")
    endif()
endforeach()

# The bundled Python (its standard library), beside the programs, where they look for it.
if(NOT EXISTS "${BUILD_DIR}/python")
    message(FATAL_ERROR "no bundled Python at ${BUILD_DIR}/python (build the app first)")
endif()
file(COPY "${BUILD_DIR}/python" DESTINATION "${dest}"
     PATTERN "__pycache__" EXCLUDE)

# Microsoft's C++ runtime, app-local (Microsoft allows these to be shipped with an app).
set(redist "$ENV{VCToolsRedistDir}")
set(crt "")
if(redist)
    file(GLOB crt LIST_DIRECTORIES true "${redist}/x64/Microsoft.VC14*.CRT")
endif()
if(NOT crt)
    file(GLOB crt LIST_DIRECTORIES true
         "C:/Program Files*/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT")
endif()
if(NOT crt)
    message(FATAL_ERROR "the Visual C++ runtime's redistributable DLLs weren't found")
endif()
list(SORT crt COMPARE NATURAL ORDER DESCENDING)  # (the newest)
list(GET crt 0 crt)
file(GLOB crt_dlls "${crt}/*.dll")
file(COPY ${crt_dlls} DESTINATION "${dest}")

# A note for whoever it's given to.
file(WRITE "${dest}/READ ME.txt"
"Setsquare ${VERSION}
==============

A 2D drawing app: sketch with constraints and dimensions, then colour it in.

To run it: double-click setsquare.exe. Nothing needs installing; keep the files in this folder
together (setsquare.exe needs the DLLs and folders beside it). To remove it, delete the folder.

The first time, Windows may say \"Windows protected your PC\" (the app isn't signed by a
publisher it knows): click \"More info\", then \"Run anyway\".

Needs 64-bit Windows 10 or 11.

Drawings are .setsq files, saved wherever you choose. Text uses the fonts installed on this PC
(a drawing from elsewhere keeps its letters' shapes, so it looks right even without its font).

setsquare-cli.exe runs Python scripts on drawings without opening a window:
    setsquare-cli script.py
    setsquare-cli --open drawing.setsq -c \"print(s.dof)\"
")

# Zipped.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}/Setsquare.zip" --format=zip Setsquare
    WORKING_DIRECTORY "${OUT}"
    RESULT_VARIABLE zipped)
if(NOT zipped EQUAL 0)
    message(FATAL_ERROR "zipping failed")
endif()
file(SIZE "${OUT}/Setsquare.zip" size)
math(EXPR mb "${size} / 1048576")
message(STATUS "Packaged: ${OUT}/Setsquare.zip (${mb} MB)")
