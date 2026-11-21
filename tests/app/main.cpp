// Catch2 runner that owns a QApplication, so tests can create widgets.

#include <catch2/catch_session.hpp>

#include "PythonHost.h"
#include "TextShapes.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>

int main(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#ifdef _WIN32
    // The offscreen platform doesn't find system fonts on Windows by itself; without them,
    // text in snapshots renders as boxes.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "\\Fonts");
    }
#endif
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);  // (app data, e.g. autosave copies, not in the real folders)
    // Settings (e.g. recent files) in a file of the tests' own, not the registry, emptied each run.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/setsquare-tests"));
    QSettings().clear();
    textshapes::addUserFonts();
    QString error;
    if (!script::initialise(&error)) {
        std::fprintf(stderr, "warning: %s (scripting tests will fail)\n", qPrintable(error));
    }
    const int result = Catch::Session().run(argc, argv);
    script::shutdown();
    return result;
}
