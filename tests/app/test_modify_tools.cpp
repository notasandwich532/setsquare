// Trim / Extend / Break / Fillet / Chamfer tools, driven with simulated input.

#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Geometry.h>
#include <core/Serialize.h>

#include <QFile>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QLineEdit>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace core;
using Catch::Approx;

namespace
{

struct Fixture
{
    Document doc;
    SketchView view{doc};
    QStringList recorded;

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y) { QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y)); }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    const Sketch& sketch() const { return doc.sketch(); }
    size_t count(EntityKind kind, bool construction = false) const
    {
        return static_cast<size_t>(std::ranges::count_if(sketch().entities(), [&](const auto& kv) {
            return kv.second.kind() == kind && kv.second.construction == construction
                && !sketch().isReference(kv.first);
        }));
    }
    QLineEdit* editor() const
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return view.findChild<QLineEdit*>("dimensionEditor");
    }
    void type(const QString& text)
    {
        QLineEdit* e = editor();
        REQUIRE(e != nullptr);
        e->selectAll();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }
    void rectangle()  // 10,10 .. 50,40, away from the axes
    {
        view.setTool(ToolKind::Rectangle);
        click(10, 10);
        click(50, 40);
    }
    Id onlyArc() const
    {
        for (const auto& [id, e] : sketch().entities()) {
            if (e.kind() == EntityKind::Arc && !e.construction) {
                return id;
            }
        }
        return kNoId;
    }
};

}  // namespace

TEST_CASE("Fillet a corner by clicking its point, then type the radius", "[modify][fillet]")
{
    Fixture f;
    f.rectangle();
    f.view.setTool(ToolKind::Fillet);
    f.click(50, 10);  // bottom-right corner
    REQUIRE(f.onlyArc() != kNoId);
    CHECK(f.sketch().arcRadius(f.onlyArc()) == Approx(5.0));  // the default
    f.type("3");
    CHECK(f.sketch().arcRadius(f.onlyArc()) == Approx(3.0).margin(1e-6));
    CHECK(f.count(EntityKind::Line, true) == 2);  // the sides that met at the corner
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(r.dof == 4);  // as the rectangle was
}

TEST_CASE("Fillet by clicking two curves", "[modify][fillet]")
{
    Fixture f;
    f.rectangle();
    f.view.modifyOptions().filletRadius = 7;
    f.view.setTool(ToolKind::Fillet);
    f.click(30, 40);  // top
    f.click(10, 25);  // left
    QTest::keyClick(f.editor(), Qt::Key_Escape);
    REQUIRE(f.onlyArc() != kNoId);
    const ArcShape a = arcShape(f.sketch(), f.onlyArc());
    CHECK(a.center.x == Approx(17.0).margin(1e-6));
    CHECK(a.center.y == Approx(33.0).margin(1e-6));
}

TEST_CASE("Chamfer variants set their dimensions", "[modify][chamfer]")
{
    Fixture f;
    f.rectangle();
    f.view.modifyOptions().chamferDistance = 6;
    f.view.modifyOptions().chamferAngle = 30;
    f.view.setTool(ToolKind::ChamferDistanceAngle);
    f.click(50, 40);  // top-right corner
    QTest::keyClick(f.editor(), Qt::Key_Escape);
    const auto angles = std::ranges::count_if(f.sketch().constraints(), [](const auto& kv) {
        return kv.second.type == ConstraintType::Angle && kv.second.value == 30.0;
    });
    CHECK(angles == 1);
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(r.dof == 4);
}

TEST_CASE("Trim, extend and break with the mouse", "[modify]")
{
    Fixture f;
    Id h = kNoId;
    f.doc.modify("lines", [&](Sketch& s) {
        h = s.addLine(10, 10, 60, 10);
        s.addLine(30, 0, 30, 20);
        s.addLine(80, 0, 80, 20);
    });
    f.view.setTool(ToolKind::Trim);
    f.move(50, 10);
    f.click(50, 10);  // the right part of h
    CHECK(f.sketch().point(f.sketch().line(h).p2).x == Approx(30.0));

    f.view.setTool(ToolKind::Extend);
    f.click(28, 10);  // near h's end: extends past the vertical it touches, to the next one
    CHECK(f.sketch().point(f.sketch().line(h).p2).x == Approx(80.0));

    f.view.setTool(ToolKind::Break);
    f.click(20, 10);
    CHECK(f.count(EntityKind::Line) == 4);  // h split at x = 30
}

TEST_CASE("Drag across lines to trim them all at once", "[modify][trim]")
{
    Fixture f;
    std::vector<Id> verticals;
    f.doc.modify("lines", [&](Sketch& s) {
        s.addLine(0, 10, 80, 10);
        for (double x : {20.0, 40.0, 60.0}) {
            verticals.push_back(s.addLine(x, 0, x, 30));
        }
    });
    f.view.setTool(ToolKind::Trim);
    // A path above the horizontal line, across the tops of all three verticals.
    QTest::mousePress(&f.view, Qt::LeftButton, {}, f.at(10, 20));
    for (int i = 1; i <= 10; ++i) {
        f.move(10 + i * 6.0, 20 + (i % 2));
    }
    QTest::mouseRelease(&f.view, Qt::LeftButton, {}, f.at(70, 20));

    for (Id v : verticals) {
        REQUIRE(f.sketch().hasEntity(v));
        const Line l = f.sketch().line(v);
        const double top = std::max(f.sketch().point(l.p1).y, f.sketch().point(l.p2).y);
        CHECK(top == Approx(10.0));  // cut back to the horizontal line
    }
    f.doc.undo();  // one step for the whole drag
    for (Id v : verticals) {
        const Line l = f.sketch().line(v);
        CHECK(std::max(f.sketch().point(l.p1).y, f.sketch().point(l.p2).y) == Approx(30.0));
    }
}

TEST_CASE("Right-click cancels what's in progress first", "[tools]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(10, 10);  // a chain has started
    QTest::mouseClick(&f.view, Qt::RightButton, {}, f.at(20, 20));
    f.click(30, 10);  // starts a new chain rather than adding a segment
    CHECK(f.count(EntityKind::Line) == 0);
}

TEST_CASE("Replaying recorded modify tools reproduces the sketch", "[modify][recorder]")
{
    Fixture f;
    f.rectangle();
    f.view.setTool(ToolKind::Fillet);
    f.click(50, 10);
    f.type("4");
    f.view.setTool(ToolKind::ChamferEqual);
    f.click(10, 40);
    f.type("3");
    f.view.setTool(ToolKind::Line);
    f.click(30, 0);
    f.click(30, 50);
    QTest::keyClick(&f.view, Qt::Key_Escape);
    f.view.setTool(ToolKind::Trim);
    f.click(30, 45);  // the bit poking out of the top
    f.view.setTool(ToolKind::TrimToConstruction);
    f.click(30, 5);  // the bit below the bottom edge, kept as construction
    CHECK(f.count(EntityKind::Line, true) >= 1);
    REQUIRE(f.recorded.size() >= 6);

    Document replay;
    script::setDocument(&replay);
    QString err;
    script::setOutput([&](const QString& t, bool e) {
        if (e) {
            err += t;
        }
    });
    script::resetConsole();
    const script::RunResult r = script::runConsole(f.recorded.join('\n'));
    script::setOutput(nullptr);
    script::setDocument(nullptr);
    INFO(f.recorded.join('\n').toStdString());
    INFO(err.toStdString());
    REQUIRE(r.ok);
    if (const QString dir = qEnvironmentVariable("DRAWAPP_DEBUG_DIR"); !dir.isEmpty()) {
        QFile script(dir + "/recorded.py");
        if (script.open(QIODevice::WriteOnly)) {
            script.write(f.recorded.join('\n').toUtf8());
        }
        QFile a(dir + "/original.json"), b(dir + "/replay.json");
        if (a.open(QIODevice::WriteOnly) && b.open(QIODevice::WriteOnly)) {
            a.write(QByteArray::fromStdString(core::toJson(f.sketch())));
            b.write(QByteArray::fromStdString(core::toJson(replay.sketch())));
        }
    }
    REQUIRE(replay.sketch().entities().size() == f.sketch().entities().size());
    REQUIRE(replay.sketch().constraints().size() == f.sketch().constraints().size());
    for (const auto& [id, e] : f.sketch().entities()) {
        REQUIRE(replay.sketch().hasEntity(id));
        CHECK(replay.sketch().entity(id).construction == e.construction);
        if (e.kind() == EntityKind::Point) {
            CHECK(replay.sketch().point(id).x == Approx(f.sketch().point(id).x).margin(1e-6));
            CHECK(replay.sketch().point(id).y == Approx(f.sketch().point(id).y).margin(1e-6));
        }
    }
}

TEST_CASE("Filleting next to a fillet uses the solid piece, not the construction original", "[modify][fillet]")
{
    // After a fillet, each side's solid piece lies on top of its original (now construction).
    // Clicking that side again must take the piece; taking the original left an extra solid
    // line behind (seen filleting all the corners of a star).
    Fixture f;
    f.rectangle();  // 10,10 .. 50,40
    f.view.setTool(ToolKind::Fillet);
    for (const auto& [a, b] : {std::pair{Vec2{30, 10}, Vec2{50, 25}}, std::pair{Vec2{50, 25}, Vec2{30, 40}},
                               std::pair{Vec2{30, 40}, Vec2{10, 25}}, std::pair{Vec2{10, 25}, Vec2{30, 10}}}) {
        f.click(a.x, a.y);
        f.click(b.x, b.y);
        QTest::keyClick(f.editor(), Qt::Key_Escape);
    }
    CHECK(f.count(EntityKind::Arc) == 4);
    CHECK(f.count(EntityKind::Line) == 4);        // one solid piece per side, between two arcs
    CHECK(f.count(EntityKind::Line, true) == 4);  // the four originals, as construction
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.redundant.empty());
}
