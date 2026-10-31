// Document file handling: save/open round trip and the modified flag.

#include "Document.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
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
        f.write("{ \"format\": \"drawing_app.sketch\", \"version\": 1, \"entities\": [ {\"id\": 1} ] }");
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
        f.write(R"({"format":"drawing_app.sketch","version":1,
                    "entities":[{"id":1,"type":"point","x":3,"y":4}],"constraints":[]})");
    }
    Document doc;
    REQUIRE(doc.open(path));
    CHECK(doc.sketch().origin() != kNoId);
    CHECK(doc.sketch().entities().size() == 2);
}
