// The M6 creation tools, driven with simulated clicks; plus replaying their recording.

#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Geometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTest>

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
        size_t n = 0;
        for (const auto& [id, e] : sketch().entities()) {
            n += e.kind() == kind && e.construction == construction;
        }
        return n;
    }
    void requireClean(int dof) const
    {
        const SolveResult& r = doc.solveResult();
        CHECK(r.ok);
        CHECK(r.redundant.empty());
        CHECK(r.dof == dof);
    }
};

}  // namespace

TEST_CASE("Point tool places points, constrained where they snap", "[shapes]")
{
    Fixture f;
    Id line = kNoId;
    f.doc.modify("line", [&](Sketch& s) { line = s.addLine(10, 10, 50, 10); });
    f.view.setTool(ToolKind::Point);
    f.click(-20, 20);  // free
    f.click(25, 10.3);  // on the line
    CHECK(f.count(EntityKind::Point) == 1 + 2 + 2);  // origin + line ends + two new
    REQUIRE(f.sketch().constraints().size() == 1);
    CHECK(f.sketch().constraints().begin()->second.refs[1] == line);
}

TEST_CASE("3-point and centre rectangles", "[shapes]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle3Point);
    f.click(10, 10);
    f.click(40, 20);
    f.click(30, 40);
    CHECK(f.count(EntityKind::Line) == 4);
    f.requireClean(5);

    f.view.setTool(ToolKind::RectangleCenter);
    f.click(-40, -20);
    f.click(-30, -10);
    CHECK(f.count(EntityKind::Line) == 8);
    CHECK(f.count(EntityKind::Line, true) == 2);  // the diagonals
    f.requireClean(9);
}

TEST_CASE("Polygon tools use the side count option", "[shapes]")
{
    Fixture f;
    f.view.setPolygonSides(5);
    f.view.setTool(ToolKind::PolygonInscribed);
    f.click(20, 20);
    f.click(30, 20);
    CHECK(f.count(EntityKind::Line) == 5);
    CHECK(f.count(EntityKind::Circle, true) == 1);
    f.requireClean(4);

    f.view.setPolygonSides(4);
    f.view.setTool(ToolKind::PolygonEdge);
    f.click(-40, -40);
    f.click(-20, -40);
    f.click(-30, -20);  // above: the square goes up
    CHECK(f.count(EntityKind::Line) == 9);
    f.requireClean(8);
}

TEST_CASE("Slot tools: straight and arc", "[shapes]")
{
    Fixture f;
    f.view.setTool(ToolKind::SlotCenterToCenter);
    f.click(10, 10);
    f.click(40, 10);
    f.click(25, 14);  // width 8
    CHECK(f.count(EntityKind::Line) == 2);
    CHECK(f.count(EntityKind::Arc) == 2);
    f.requireClean(5);

    f.view.setTool(ToolKind::SlotArcCenter);
    f.click(-30, 0);
    f.click(-10, 0);
    f.move(-16, 14);
    f.move(-30, 20);
    f.click(-30, 20);
    f.click(-30, 23);  // width 6
    CHECK(f.count(EntityKind::Arc) == 2 + 4);
    CHECK(f.count(EntityKind::Arc, true) == 1);  // the centreline
    f.requireClean(11);
}

TEST_CASE("Tangent arc continues off a line end", "[shapes]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(10, 10);
    f.click(40, 10);
    QTest::keyClick(&f.view, Qt::Key_Escape);
    f.view.setTool(ToolKind::ArcTangent);
    f.click(40, 10);
    f.click(50, 20);
    REQUIRE(f.count(EntityKind::Arc) == 1);
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.redundant.empty());
}

TEST_CASE("2-tangent circle sits in the corner the mouse is in", "[shapes]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle);
    f.click(10, 10);
    f.click(60, 40);
    f.view.setTool(ToolKind::CircleTangent2);
    f.click(35, 10);  // bottom side
    f.click(10, 25);  // left side
    f.move(16, 16);
    f.click(16, 16);
    REQUIRE(f.count(EntityKind::Circle) == 1);
    for (const auto& [id, e] : f.sketch().entities()) {
        if (e.kind() == EntityKind::Circle) {
            const Point& c = f.sketch().point(f.sketch().circle(id).center);
            CHECK(c.x - 10 == Approx(f.sketch().circle(id).radius).margin(1e-6));
            CHECK(c.y - 10 == Approx(f.sketch().circle(id).radius).margin(1e-6));
        }
    }
}

TEST_CASE("Replaying recorded shape tools reproduces the sketch", "[shapes][recorder]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle3Point);
    f.click(10, 10);
    f.click(40, 20);
    f.click(30, 40);
    f.view.setPolygonSides(7);
    f.view.setTool(ToolKind::PolygonCircumscribed);
    f.click(-30, 30);
    f.click(-20, 30);
    f.view.setTool(ToolKind::SlotArc3Point);
    f.click(-40, -20);
    f.click(-10, -20);
    f.click(-25, -10);
    f.click(-25, -8);
    f.view.setTool(ToolKind::Circle3Point);
    f.click(50, -30);
    f.click(60, -20);
    f.click(50, -10);
    REQUIRE(f.recorded.size() >= 4);

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
    REQUIRE(replay.sketch().entities().size() == f.sketch().entities().size());
    REQUIRE(replay.sketch().constraints().size() == f.sketch().constraints().size());
    for (const auto& [id, e] : f.sketch().entities()) {
        REQUIRE(replay.sketch().hasEntity(id));
        if (e.kind() == EntityKind::Point) {
            CHECK(replay.sketch().point(id).x == Approx(f.sketch().point(id).x).margin(1e-6));
            CHECK(replay.sketch().point(id).y == Approx(f.sketch().point(id).y).margin(1e-6));
        }
    }
}
