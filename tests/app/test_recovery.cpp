// Autosave and crash recovery: unsaved work copied aside, removed once it's safe, and offered back
// after a session that didn't close.

#include "Document.h"
#include "MainWindow.h"
#include "Recovery.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>

using namespace core;

namespace
{
size_t circles(const Document& doc)
{
    return static_cast<size_t>(std::ranges::count_if(doc.sketch().entities(), [](const auto& kv) {
        return kv.second.kind() == EntityKind::Circle;
    }));
}
}  // namespace

TEST_CASE("Recovery: unsaved work is copied aside; saved, the copy goes", "[recovery]")
{
    QTemporaryDir dir;
    Document doc;
    Recovery recovery(doc, dir.path());
    recovery.writeNow();
    CHECK(!QFile::exists(recovery.copyPath()));  // (nothing unsaved)

    doc.modify("circle", [](Sketch& s) { s.addCircle(10, 10, 5); });
    REQUIRE(doc.isModified());
    recovery.writeNow();
    REQUIRE(QFile::exists(recovery.copyPath()));
    Document copy;
    REQUIRE(copy.open(recovery.copyPath()));
    CHECK(circles(copy) == 1);
    CHECK(doc.isModified());        // (the document's own state untouched)
    CHECK(doc.filePath().isEmpty());
    CHECK(Recovery::left(dir.path()).empty());  // (this session holds it: not left over)

    QTemporaryDir files;
    REQUIRE(doc.save(files.filePath(QStringLiteral("drawing.setsq"))));
    CHECK(!QFile::exists(recovery.copyPath()));
}

TEST_CASE("Recovery: a clean close takes its copy with it", "[recovery]")
{
    QTemporaryDir dir;
    Document doc;
    QString copy;
    {
        Recovery recovery(doc, dir.path());
        doc.modify("circle", [](Sketch& s) { s.addCircle(10, 10, 5); });
        recovery.writeNow();
        copy = recovery.copyPath();
        REQUIRE(QFile::exists(copy));
    }
    CHECK(!QFile::exists(copy));
    CHECK(Recovery::left(dir.path()).empty());
}

TEST_CASE("Recovery: after a crash, the copy is offered back as unsaved work on the original", "[recovery]")
{
    QTemporaryDir dir, files;
    const QString original = files.filePath(QStringLiteral("drawing.setsq"));
    {
        Document doc;
        Recovery recovery(doc, dir.path());
        doc.modify("rectangle", [](Sketch& s) { build::rectangle(s, s.addPoint(0, 0), s.addPoint(40, 20)); });
        REQUIRE(doc.save(original));
        doc.modify("circle", [](Sketch& s) { s.addCircle(10, 10, 5); });  // (unsaved)
        recovery.writeNow();
        recovery.abandonForTest();  // the process "dies": the copy stays
    }
    const std::vector<Recovery::Found> found = Recovery::left(dir.path());
    REQUIRE(found.size() == 1);
    CHECK(found[0].original == original);
    CHECK(found[0].when.isValid());

    Document doc;
    REQUIRE(doc.openRecovered(found[0].copy, found[0].original));
    CHECK(circles(doc) == 1);  // the unsaved circle is back
    CHECK(doc.filePath() == original);
    CHECK(doc.isModified());
    Document onDisk;
    REQUIRE(onDisk.open(original));
    CHECK(circles(onDisk) == 0);  // (the file itself untouched)

    Recovery::remove(found[0]);
    CHECK(Recovery::left(dir.path()).empty());
    CHECK(!QFile::exists(found[0].copy));
}

TEST_CASE("Recovery: the window offers it at start-up; Recover brings it back, Discard drops it", "[recovery]")
{
    QTemporaryDir dir;
    const bool recover = GENERATE(true, false);
    {
        Document doc;
        Recovery recovery(doc, dir.path());
        doc.modify("circle", [](Sketch& s) { s.addCircle(10, 10, 5); });
        recovery.writeNow();
        recovery.abandonForTest();
    }
    REQUIRE(Recovery::left(dir.path()).size() == 1);
    MainWindow window;
    window.show();
    QTimer::singleShot(0, [recover] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box != nullptr);
        for (QAbstractButton* b : box->buttons()) {
            if (b->text() == (recover ? QStringLiteral("Recover") : QStringLiteral("Discard"))) {
                b->click();
            }
        }
    });
    CHECK(window.offerRecovery(dir.path()) == recover);
    CHECK(Recovery::left(dir.path()).empty());  // (either way: dealt with)
    CHECK(window.windowTitle().contains(recover ? QStringLiteral("Untitled*") : QStringLiteral("Untitled -")));
}
TEST_CASE("Recovery: a session that ended before writing anything leaves only a lock, cleared away", "[recovery]")
{
    QTemporaryDir dir;
    QString lockPath;
    {
        Document doc;
        Recovery recovery(doc, dir.path());
        lockPath = QFileInfo(recovery.copyPath()).absolutePath() + QLatin1Char('/')
                 + QFileInfo(recovery.copyPath()).completeBaseName() + QStringLiteral(".lock");
        REQUIRE(QFile::exists(lockPath));
        Recovery other(doc, dir.path());  // (a running one: its lock stays)
        CHECK(Recovery::left(dir.path()).empty());
        CHECK(QFile::exists(lockPath));
    }
    CHECK(!QFile::exists(lockPath));  // (a clean close lets go of it)

    // A crash's: the lock of a process that's gone (QLockFile's own format), with no copy.
    const QString dead = dir.filePath(QStringLiteral("crashed.lock"));
    QFile file(dead);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("4000000000\nsetsquare\n" + QSysInfo::machineHostName().toUtf8() + "\n");
    file.close();
    CHECK(Recovery::left(dir.path()).empty());
    CHECK(!QFile::exists(dead));
}