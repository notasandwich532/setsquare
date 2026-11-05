// The M6 creation tools, driven with simulated clicks; plus replaying their recording.

#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Geometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QLineEdit>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

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
            n += e.kind() == kind && e.construction == construction && !sketch().isReference(id);
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
    CHECK(f.count(EntityKind::Point) == 2 + 2);  // line ends + two new
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

TEST_CASE("Typing a number sets the polygon's side count", "[shapes]")
{
    Fixture f;
    f.view.setTool(ToolKind::PolygonCircumscribed);
    QTest::keyClick(&f.view, Qt::Key_8);
    CHECK(f.view.polygonSides() == 8);
    QTest::keyClicks(&f.view, "12");  // two digits in quick succession
    CHECK(f.view.polygonSides() == 12);
    QTest::keyClick(&f.view, Qt::Key_Minus);
    CHECK(f.view.polygonSides() == 11);
    QTest::keyClick(&f.view, Qt::Key_Plus);
    QTest::keyClick(&f.view, Qt::Key_Plus);
    CHECK(f.view.polygonSides() == 13);
    f.click(0, 0);
    f.click(10, 0);
    CHECK(f.count(EntityKind::Line) == 13);
    f.requireClean(2);  // centred on the origin: size and rotation left
}

TEST_CASE("Dimension the angle to the X axis", "[shapes][dimension][axes]")
{
    Fixture f;
    Id l = kNoId;
    f.doc.modify("line", [&](Sketch& s) { l = s.addLine(20, 10, 40, 20); });
    f.view.setTool(ToolKind::Dimension);
    f.click(30, 15);   // the line
    f.click(-60, 0);   // the X axis, well away from anything else
    f.click(45, 12);   // place
    const auto& cs = f.sketch().constraints();
    REQUIRE(cs.size() == 1);
    const Constraint& c = cs.begin()->second;
    CHECK(c.type == ConstraintType::Angle);
    CHECK(std::ranges::find(c.refs, f.sketch().xAxis()) != c.refs.end());
    CHECK(c.value == Approx(std::atan2(10.0, 20.0) * 180 / std::numbers::pi));
}

TEST_CASE("Placing an angle dimension inside or outside a corner picks that angle", "[dimension][angle]")
{
    // A corner at (20, 20): one side going right, one going up-right at 60 degrees.
    for (auto [placeX, placeY, expected] : {std::tuple{32.0, 24.0, 60.0}, std::tuple{12.0, 28.0, 120.0}}) {
        Fixture f;
        Id a = kNoId, b = kNoId;
        f.doc.modify("corner", [&](Sketch& s) {
            const Id corner = s.addPoint(20, 20);
            a = s.addLine(corner, s.addPoint(40, 20));
            b = s.addLine(corner, s.addPoint(20 + 20 * 0.5, 20 + 20 * std::sqrt(3.0) / 2));
        });
        f.view.setTool(ToolKind::Dimension);
        f.click(30, 20);  // side a
        f.click(25, 20 + 5 * std::sqrt(3.0));  // side b
        f.move(placeX, placeY);
        f.click(placeX, placeY);
        const auto& cs = f.sketch().constraints();
        REQUIRE(cs.size() == 1);
        CHECK(cs.begin()->second.value == Approx(expected).margin(1e-6));
    }
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

    f.view.setTool(ToolKind::SlotArcCenter);  // (off the X axis, which would snap the centre)
    f.click(-30, 5);
    f.click(-10, 5);
    f.move(-16, 19);
    f.move(-30, 25);
    f.click(-30, 25);
    f.click(-30, 28);  // width 6
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

TEST_CASE("Ellipse tool: centre, an axis end, then the other axis", "[shapes][curves]")
{
    Fixture f;
    f.view.setTool(ToolKind::Ellipse);
    f.click(10, 10);
    f.click(40, 10);
    f.move(25, 25);
    f.click(25, 25);  // the other axis: 15 out from the first
    REQUIRE(f.count(EntityKind::Ellipse) == 1);
    Id ellipse = kNoId;
    for (const auto& [id, e] : f.sketch().entities()) {
        if (e.kind() == EntityKind::Ellipse) {
            ellipse = id;
        }
    }
    const auto ax = f.sketch().ellipseAxes(ellipse);
    const Point& c = f.sketch().point(ax[0]);
    const Point& a = f.sketch().point(ax[1]);
    const Point& b = f.sketch().point(ax[2]);
    CHECK(std::hypot(a.x - c.x, a.y - c.y) == Approx(30).margin(0.5));
    CHECK(std::hypot(b.x - c.x, b.y - c.y) == Approx(15).margin(0.5));
    f.requireClean(5);
    CHECK(f.recorded.last().contains("s.ellipse("));
}

TEST_CASE("Spline tool: click points, Enter to finish; the first point again closes it", "[shapes][curves]")
{
    Fixture f;
    f.view.setTool(ToolKind::SplineFit);
    f.click(0, 0);
    f.click(10, 10);
    f.click(20, 0);
    f.click(30, 10);
    QTest::keyClick(&f.view, Qt::Key_Backspace);  // takes (30, 10) back
    f.click(30, 5);
    QTest::keyClick(&f.view, Qt::Key_Return);
    REQUIRE(f.count(EntityKind::Spline) == 1);
    Id fit = kNoId;
    for (const auto& [id, e] : f.sketch().entities()) {
        if (e.kind() == EntityKind::Spline) {
            fit = id;
        }
    }
    const Spline& sp = f.sketch().spline(fit);
    CHECK(sp.points.size() == 4);
    CHECK(sp.kind == SplineKind::Fit);
    CHECK_FALSE(sp.closed);
    CHECK(f.sketch().point(sp.points.back()).y == Approx(5));
    CHECK(f.recorded.last().contains("s.spline("));

    // A closed control-point spline: back to the first point.
    f.view.setTool(ToolKind::SplineControl);
    f.click(50, 0);
    f.click(60, 10);
    f.click(70, 0);
    f.click(60, -10);
    f.click(50, 0);
    REQUIRE(f.count(EntityKind::Spline) == 2);
    Id ring = kNoId;
    for (const auto& [id, e] : f.sketch().entities()) {
        if (e.kind() == EntityKind::Spline && id != fit) {
            ring = id;
        }
    }
    CHECK(f.sketch().spline(ring).closed);
    CHECK(f.sketch().spline(ring).kind == SplineKind::Control);
    CHECK(f.sketch().spline(ring).points.size() == 4);
    f.requireClean(11);  // (16, less the origin and three points snapped onto the x axis)

    // Esc throws a spline in progress away.
    f.view.setTool(ToolKind::SplineFit);
    f.click(0, 40);
    f.click(10, 50);
    QTest::keyClick(&f.view, Qt::Key_Escape);
    CHECK(f.count(EntityKind::Spline) == 2);
}

TEST_CASE("Dimension tool on an ellipse: the axis nearer the click", "[shapes][curves]")
{
    Fixture f;
    f.doc.edit("ellipse", [&](Editor& ed) { ed.ellipse(ed.point(0, 0), ed.point(30, 0), ed.point(0, 15)); });
    f.view.setTool(ToolKind::Dimension);
    f.click(30 * std::cos(0.35), 15 * std::sin(0.35));  // on the curve, nearer the major axis
    f.click(45, 10);    // place
    f.view.setTool(ToolKind::Dimension);
    f.click(30 * std::cos(1.3), 15 * std::sin(1.3));  // nearer the minor axis
    f.click(10, 30);
    std::vector<ConstraintType> types;
    for (const auto& [id, c] : f.sketch().constraints()) {
        types.push_back(c.type);
    }
    CHECK(std::ranges::count(types, ConstraintType::Radius) == 1);
    CHECK(std::ranges::count(types, ConstraintType::Distance) == 1);
}

TEST_CASE("Fillet a line and a spline by clicking their corner; extend an elliptical arc", "[shapes][curves]")
{
    Fixture f;
    Id line = kNoId, spline = kNoId;
    f.doc.edit("curves", [&](Editor& ed) {
        const Id a = ed.point(-40, 20), b = ed.point(-20, 20);
        line = ed.line(a, b);
        spline = ed.spline({b, ed.point(-10, 30), ed.point(0, 20), ed.point(10, 30)}, SplineKind::Fit, false);
    });
    f.view.setTool(ToolKind::Fillet);
    f.click(-20, 20);  // the corner point
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (auto* e = f.view.findChild<QLineEdit*>("dimensionEditor")) {
        e->selectAll();
        QTest::keyClicks(e, "3");
        QTest::keyClick(e, Qt::Key_Return);
    }
    CHECK(f.count(EntityKind::Arc) == 1);
    CHECK(f.count(EntityKind::Spline) == 1);   // the kept piece
    CHECK(f.count(EntityKind::Spline, true) == 1);  // the original, now construction
    CHECK(f.doc.solveResult().ok);
    CHECK(f.doc.solveResult().redundant.empty());

    // An elliptical arc carries on round its ellipse to the line beyond its end.
    Id arc = kNoId;
    f.doc.edit("arc", [&](Editor& ed) {
        arc = ed.ellipseArc(ed.point(0, -30), ed.point(20, -30), ed.point(0, -20), ed.point(20, -30), ed.point(0, -20));
        ed.line(ed.point(-15, -50), ed.point(-15, -10));
    });
    f.view.setTool(ToolKind::Extend);
    f.click(2, -20.2);  // near its end (the top)
    const Point& end = f.sketch().point(f.sketch().ellipseArc(arc).end);
    CHECK(end.x == Approx(-15).margin(1e-6));
}
