#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("drawing_app");
    QApplication::setOrganizationName("drawing_app");

    QCommandLineParser parser;
    parser.setApplicationDescription("Parametric 2D drawing app");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "Sketch (.dsk) to open");
    parser.process(app);

    theme::applyDark(app);

    MainWindow window;
    window.show();
    if (const QStringList files = parser.positionalArguments(); !files.isEmpty()) {
        // After the first layout, so fitting the view uses the real canvas size.
        QTimer::singleShot(0, &window, [&window, path = files.first()] { window.openFile(path); });
    }
    return QApplication::exec();
}
