// Document file handling: save/open round trip and the modified flag.

#include "Document.h"
#include "PythonHost.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <cmath>
#include <algorithm>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace core;

TEST_CASE("Save and open round-trip a sketch", "[document][files]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("bracket.dsk");

    Document a;
    a.modify("draw", [](Sketch& s) {
        const Id l = s.addLine(s.origin(), s.addPoint(40, 0));
        s.addConstraint(ConstraintType::Horizontal, {l});
        s.addConstraint(ConstraintType::Distance, {l}, 40.0);
    });
    QString error;
    REQUIRE(a.save(path, &error));
    CHECK(error.isEmpty());
    CHECK(a.filePath() == path);

    Document b;
    REQUIRE(b.open(path, &error));
    CHECK(b.sketch().entities().size() == a.sketch().entities().size());
    CHECK(b.sketch().constraints().size() == 2);
    CHECK(b.sketch().origin() == a.sketch().origin());
    CHECK(b.solveResult().dof == 0);  // origin + horizontal + length
    CHECK_FALSE(b.isModified());
    CHECK_FALSE(b.canUndo());
}

TEST_CASE("Modified flag follows edits, saves and undo", "[document][files]")
{
    QTemporaryDir dir;
    Document doc;
    QSignalSpy fileState(&doc, &Document::fileStateChanged);
    CHECK_FALSE(doc.isModified());

    doc.modify("p", [](Sketch& s) { s.addPoint(1, 1); });
    CHECK(doc.isModified());
    REQUIRE(doc.save(dir.filePath("x.dsk")));
    CHECK_FALSE(doc.isModified());

    doc.modify("q", [](Sketch& s) { s.addPoint(2, 2); });
    CHECK(doc.isModified());
    doc.undo();  // back to exactly what was saved
    CHECK_FALSE(doc.isModified());
    doc.undo();  // before the save
    CHECK(doc.isModified());
    CHECK(fileState.count() > 0);
}

TEST_CASE("Opening a bad file reports an error and keeps the current sketch", "[document][files]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath("broken.dsk");
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{ \"format\": \"setsquare.sketch\", \"version\": 1, \"entities\": [ {\"id\": 1} ] }");
    }
    Document doc;
    doc.modify("p", [](Sketch& s) { s.addPoint(5, 5); });
    const size_t before = doc.sketch().entities().size();

    QString error;
    CHECK_FALSE(doc.open(path, &error));
    CHECK_FALSE(error.isEmpty());
    CHECK(doc.sketch().entities().size() == before);
    CHECK_FALSE(doc.open(dir.filePath("does-not-exist.dsk"), &error));
}

TEST_CASE("Files without an origin get one when opened", "[document][files]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath("old.dsk");
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"({"format":"setsquare.sketch","version":1,
                    "entities":[{"id":1,"type":"point","x":3,"y":4}],"constraints":[]})");
    }
    Document doc;
    REQUIRE(doc.open(path));
    CHECK(doc.sketch().origin() != kNoId);
    CHECK(doc.sketch().entities().size() == 6);  // the point, plus origin and axes
}

TEST_CASE("The command history follows edits, undo and redo, and is saved", "[document][history]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Document doc;
    Id line = kNoId;
    doc.edit("line", [&](Editor& ed) { line = ed.line(ed.point(0, 0), ed.point(30, 5)); });
    REQUIRE(doc.history().size() == 1);
    CHECK(doc.history()[0].contains("s.line("));
    doc.addConstraint(ConstraintType::Horizontal, {line});
    doc.addConstraint(ConstraintType::Distance, {line}, 30.0);
    CHECK(doc.history().size() == 3);

    doc.undo();  // the undone command comes off
    CHECK(doc.history().size() == 2);
    doc.redo();
    CHECK(doc.history().size() == 3);
    CHECK(doc.history().last().contains("s.distance("));

    // Calls made from Python go in too, as the same kind of line.
    script::setDocument(&doc);
    script::resetConsole();
    REQUIRE(script::runConsole(QStringLiteral("s.circle((50, 0), 4)")).ok);
    script::setDocument(nullptr);
    REQUIRE(doc.history().size() == 4);
    CHECK(doc.history().last().contains("s.circle("));

    const QString path = dir.filePath("history.dsk");
    REQUIRE(doc.save(path));
    Document loaded;
    REQUIRE(loaded.open(path));
    CHECK(loaded.history() == doc.history());

    // Replaying it rebuilds the drawing.
    Document rebuilt;
    script::setDocument(&rebuilt);
    script::resetConsole();
    const script::RunResult r = script::runConsole(doc.history().join('\n'));
    script::setDocument(nullptr);
    REQUIRE(r.ok);
    CHECK(rebuilt.sketch().entities().size() == doc.sketch().entities().size());
    CHECK(rebuilt.sketch().constraints().size() == doc.sketch().constraints().size());
}
TEST_CASE("A refused change names what's in its way, fixed geometry included", "[document][blockers]")
{
    // A fixed line, and a vertical line up from its end: the far corner can't come closer to
    // the fixed line's start than 10, because the fixed line can't move.
    Document doc;
    Id fixedLine = kNoId, dim = kNoId;
    doc.modify("draw", [&](Sketch& s) {
        const Id a = s.addPoint(0, 0), b = s.addPoint(10, 0), c = s.addPoint(10, 10);
        fixedLine = s.addLine(a, b);
        s.entity(fixedLine).fixed = true;
        const Id up = s.addLine(b, c);
        s.addConstraint(ConstraintType::Vertical, {up});
        dim = s.addConstraint(ConstraintType::Distance, {a, c}, std::sqrt(200.0));
    });
    REQUIRE(doc.solveResult().dof == 0);

    QSignalSpy blocked(&doc, &Document::blocked);
    QString error;
    CHECK_FALSE(doc.setDimensionValue(dim, 5.0, &error));
    INFO(error.toStdString());
    CHECK(error.contains("fixed l" + QString::number(fixedLine)));
    REQUIRE(blocked.size() == 1);
    const auto items = blocked.first().first().value<std::vector<Id>>();
    CHECK(std::ranges::find(items, fixedLine) != items.end());
}