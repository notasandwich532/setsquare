#include "FileAssociation.h"
#include "Icons.h"
#include "MainWindow.h"
#include "PythonHost.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <QMainWindow>
#include <QStatusBar>

#include <cstdio>
#include <exception>

namespace
{
// An error that escapes an event handler (a bug) is reported rather than ending the app: Qt can't
// carry exceptions through its event loop, so without this any one is a crash, and the drawing
// with it.
class SafeApplication : public QApplication
{
public:
    using QApplication::QApplication;

    bool notify(QObject* receiver, QEvent* event) override
    {
        try {
            return QApplication::notify(receiver, event);
        }
        catch (const std::exception& e) {
            report(QString::fromUtf8(e.what()));
        }
        catch (...) {
            report(QStringLiteral("unknown error"));
        }
        return false;
    }

private:
    static void report(const QString& what)
    {
        std::fprintf(stderr, "Internal error (the app carried on): %s\n", qPrintable(what));
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* window = qobject_cast<QMainWindow*>(w)) {
                window->statusBar()->showMessage(QObject::tr("Internal error (please report it): %1").arg(what), 8000);
            }
        }
    }
};
}  // namespace

int main(int argc, char* argv[])
{
    SafeApplication app(argc, argv);
    QApplication::setApplicationName("Setsquare");
    QApplication::setOrganizationName("Setsquare");
    QApplication::setWindowIcon(icons::appIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription("Parametric 2D drawing app");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "Sketch (.setsq) to open");
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
            else {
                window.offerRecovery();  // (a crash's unsaved work; with a file to open, offered next time)
            }
            fileassoc::offerAtStartup(&window);  // (asked once: double-clicking .setsq files opens them here)
            if (!scriptPath.isEmpty()) {
                window.runScript(scriptPath);
            }
        });
        result = QApplication::exec();
    }
    script::shutdown();  // after the window (and its console) are gone
    return result;
}
