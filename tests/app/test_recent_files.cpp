// File > Open Recent: what was opened or saved, newest first; one that's gone is said so and dropped.

#include "Document.h"
#include "MainWindow.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

using namespace core;

namespace
{
// A drawing saved at `path` (a rectangle), by another document.
void saveDrawing(const QString& path)
{
    Document doc;
    doc.modify("rectangle", [](Sketch& s) { build::rectangle(s, s.addPoint(0, 0), s.addPoint(40, 20)); });
    REQUIRE(doc.save(path));
}

QStringList shown(MainWindow& window)
{
    auto* menu = window.findChild<QMenu*>("recentFilesMenu");
    REQUIRE(menu != nullptr);
    emit menu->aboutToShow();
    QStringList out;
    for (QAction* a : menu->actions()) {
        if (!a->isSeparator()) {
            out << a->text();
        }
    }
    return out;
}

QAction* entry(MainWindow& window, const QString& name)
{
    auto* menu = window.findChild<QMenu*>("recentFilesMenu");
    emit menu->aboutToShow();
    for (QAction* a : menu->actions()) {
        if (a->text().endsWith(name)) {
            return a;
        }
    }
    return nullptr;
}

QString full(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
}  // namespace

TEST_CASE("Open Recent: what's opened or saved, newest first, once each, ten at most", "[recent]")
{
    QSettings().remove("recentFiles");
    QTemporaryDir dir;
    MainWindow window;
    CHECK(shown(window) == QStringList{"(none yet)"});

    const QString a = dir.filePath("a.setsq"), b = dir.filePath("b.setsq");
    saveDrawing(a);
    saveDrawing(b);
    REQUIRE(window.openFile(a));
    REQUIRE(window.openFile(b));
    CHECK(MainWindow::recentFiles() == QStringList{full(b), full(a)});
    REQUIRE(window.openFile(a));  // (again: to the top, not twice)
    CHECK(MainWindow::recentFiles() == QStringList{full(a), full(b)});
    const QStringList menu = shown(window);
    REQUIRE(menu.size() == 3);
    CHECK(menu[0] == "&1  a.setsq");
    CHECK(menu[1] == "&2  b.setsq");
    CHECK(menu[2] == "&Clear Recent");

    for (int i = 0; i < 12; ++i) {
        MainWindow::addRecentFile(dir.filePath(QStringLiteral("n%1.setsq").arg(i)));
    }
    CHECK(MainWindow::recentFiles().size() == MainWindow::kRecentFiles);
    CHECK(MainWindow::recentFiles().front().endsWith("n11.setsq"));
}

TEST_CASE("Open Recent: saving puts it there; choosing one opens it; Clear Recent empties it", "[recent]")
{
    QSettings().remove("recentFiles");
    QTemporaryDir dir;
    const QString other = dir.filePath("other.setsq");
    saveDrawing(other);
    MainWindow::addRecentFile(other);

    MainWindow window;
    auto* doc = window.findChild<Document*>();
    REQUIRE(doc != nullptr);
    const QString saved = dir.filePath("saved.setsq");
    REQUIRE(doc->save(saved));  // (as File > Save, or doc.save from Python)
    CHECK(MainWindow::recentFiles().front() == full(saved));

    QAction* pick = entry(window, "other.setsq");
    REQUIRE(pick != nullptr);
    pick->trigger();
    CHECK(full(doc->filePath()) == full(other));
    CHECK(MainWindow::recentFiles().front() == full(other));

    QAction* clear = entry(window, "Clear Recent");
    REQUIRE(clear != nullptr);
    clear->trigger();
    CHECK(MainWindow::recentFiles().isEmpty());
}

TEST_CASE("Open Recent: one that's gone is said so and taken off; the drawing open stays", "[recent]")
{
    QSettings().remove("recentFiles");
    QTemporaryDir dir;
    const QString gone = dir.filePath("gone.setsq"), kept = dir.filePath("kept.setsq");
    saveDrawing(gone);
    saveDrawing(kept);
    MainWindow window;
    REQUIRE(window.openFile(gone));
    REQUIRE(window.openFile(kept));
    REQUIRE(QFile::remove(gone));

    QString said;
    QTimer::singleShot(0, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box != nullptr);
        said = box->text();
        box->accept();
    });
    QAction* pick = entry(window, "gone.setsq");
    REQUIRE(pick != nullptr);
    pick->trigger();
    CHECK(said.contains("isn't there any more"));
    CHECK(MainWindow::recentFiles() == QStringList{full(kept)});
    CHECK(window.windowTitle().startsWith("kept.setsq"));
}