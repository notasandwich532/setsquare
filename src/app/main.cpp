#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("drawing_app");
    QApplication::setOrganizationName("drawing_app");

    theme::applyDark(app);

    MainWindow window;
    window.show();
    return QApplication::exec();
}
