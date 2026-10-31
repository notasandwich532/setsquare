// Catch2 runner that owns a QApplication, so tests can create widgets.

#include <catch2/catch_session.hpp>

#include <QApplication>

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
    return Catch::Session().run(argc, argv);
}
