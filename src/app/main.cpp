#include "MainWindow.h"
#include "PythonHost.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("drawing_app");
    QApplication::setOrganizationName("drawing_app");

    QCommandLineParser parser;
    parser.setApplicationDescription("Parametric 2D drawing app");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "Sketch (.dsk) to open");
    const QCommandLineOption scriptOption("script", "Run a Python script once the window is up.", "file.py");
    parser.addOption(scriptOption);
    parser.process(app);

    theme::applyDark(app);

    // Scripting is optional: without the bundled Python the app still works, just without it.
    QString pythonError;
    if (!script::initialise(&pythonError)) {
        std::fprintf(stderr, "%s\n", qPrintable(pythonError));
    }

    int result = 0;
    {
        MainWindow window;
        window.show();
        const QStringList files = parser.positionalArguments();
        const QString scriptPath = parser.value(scriptOption);
        // After the first layout, so fitting the view uses the real canvas size.
        QTimer::singleShot(0, &window, [&window, files, scriptPath] {
            if (!files.isEmpty()) {
                window.openFile(files.first());
            }
            if (!scriptPath.isEmpty()) {
                window.runScript(scriptPath);
            }
        });
        result = QApplication::exec();
    }
    script::shutdown();  // after the window (and its console) are gone
    return result;
}
