// End-to-end tests of the sketch tools: real SketchView + Document, driven by simulated
// mouse/keyboard input on the offscreen platform.

#include "Document.h"
#include "SketchView.h"

#include <core/Geometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <QDir>
#include <QRegularExpression>
#include <QTest>

#include <cmath>
#include <numbers>

using namespace core;
using Catch::Approx;

namespace
{

struct Fixture
{
    Document doc;
    SketchView view{doc};

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();  // origin at the centre, 4 px per mm
    }

    // With DRAWAPP_SNAPSHOT_DIR set, each test leaves a picture of the canvas behind for eyeballing.
    ~Fixture()
    {
        const QString dir = qEnvironmentVariable("DRAWAPP_SNAPSHOT_DIR");
        if (!dir.isEmpty()) {
            QString name = QString::fromStdString(Catch::getResultCapture().getCurrentTestName());
            name.replace(QRegularExpression("[^A-Za-z0-9]+"), "_");
            view.grab().save(QDir(dir).filePath(name + ".png"));
        }
    }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y, Qt::MouseButton b = Qt::LeftButton) { QTest::mouseClick(&view, b, {}, at(x, y)); }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    void press(double x, double y) { QTest::mousePress(&view, Qt::LeftButton, {}, at(x, y)); }
    void release(double x, double y) { QTest::mouseRelease(&view, Qt::LeftButton, {}, at(x, y)); }
    void key(Qt::Key k) { QTest::keyClick(&view, k); }

    // A drag with intermediate moves, as a real mouse would produce.
    void drag(double x0, double y0, double x1, double y1, int steps = 8)
    {
        press(x0, y0);
        for (int i = 1; i <= steps; ++i) {
            const double t = double(i) / steps;
            move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
        }
        release(x1, y1);
    }

    const Sketch& sketch() const { return doc.sketch(); }

    std::vector<Id> ofKind(EntityKind kind) const
    {
        std::vector<Id> out;
        for (const auto& [id, e] : sketch().entities()) {
            if (e.kind() == kind) {
                out.push_back(id);
            }
        }
        return out;
    }

    Vec2 pos(Id point) const
    {
        const Point& p = sketch().point(point);
        return {p.x, p.y};
    }
};

}  // namespace

TEST_CASE("Line tool draws a connected chain until Esc", "[tools][line]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(0, 0);
    f.click(20, 0);
    f.click(20, 10);
    f.key(Qt::Key_Escape);

    REQUIRE(f.ofKind(EntityKind::Line).size() == 2);
    CHECK(f.ofKind(EntityKind::Point).size() == 3);  // middle point is shared

    // Esc ended the chain: the next click starts a new one instead of adding a segment.
    f.click(50, 50);
    CHECK(f.ofKind(EntityKind::Line).size() == 2);
    CHECK(f.view.toolKind() == ToolKind::Line);

    // A second Esc (nothing in progress) returns to the select tool.
    f.key(Qt::Key_Escape);
    f.key(Qt::Key_Escape);
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Clicking the first point closes the loop", "[tools][line]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(0, 0);
    f.click(20, 0);
    f.click(20, 20);
    f.click(0.5, 0.5);  // within snap distance of the start
    CHECK(f.ofKind(EntityKind::Line).size() == 3);
    CHECK(f.ofKind(EntityKind::Point).size() == 3);

    f.click(40, 40);  // chain ended: this only starts a new one
    CHECK(f.ofKind(EntityKind::Line).size() == 3);
}

TEST_CASE("New geometry snaps onto existing points", "[tools][snap]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(0, 0);
    f.click(20, 0);
    f.key(Qt::Key_Escape);
    f.click(20.5, 0.5);  // near the previous end
    f.click(20, 15);
    f.key(Qt::Key_Escape);

    const auto lines = f.ofKind(EntityKind::Line);
    REQUIRE(lines.size() == 2);
    CHECK(f.sketch().line(lines[0]).p2 == f.sketch().line(lines[1]).p1);
}

TEST_CASE("Rectangle tool adds four constrained sides", "[tools][rectangle]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle);
    f.click(0, 0);
    f.click(30, 20);

    CHECK(f.ofKind(EntityKind::Line).size() == 4);
    CHECK(f.ofKind(EntityKind::Point).size() == 4);
    CHECK(f.sketch().constraints().size() == 4);
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.dof == 4);
}

TEST_CASE("Circle tool: centre then radius", "[tools][circle]")
{
    Fixture f;
    f.view.setTool(ToolKind::Circle);
    f.click(5, 5);
    f.click(15, 5);
    const auto circles = f.ofKind(EntityKind::Circle);
    REQUIRE(circles.size() == 1);
    CHECK(f.sketch().circle(circles[0]).radius == Approx(10.0));
}

TEST_CASE("Centre arc follows the mouse direction", "[tools][arc]")
{
    SECTION("counter-clockwise")
    {
        Fixture f;
        f.view.setTool(ToolKind::ArcCenter);
        f.click(0, 0);
        f.click(10, 0);
        f.move(9, 4);
        f.move(7, 7);
        f.move(0, 10);
        f.click(0, 10);
        const auto arcs = f.ofKind(EntityKind::Arc);
        REQUIRE(arcs.size() == 1);
        const ArcShape a = arcShape(f.sketch(), arcs[0]);
        CHECK(a.radius == Approx(10.0));
        CHECK(a.startAngle == Approx(0.0).margin(1e-9));
        CHECK(a.sweep == Approx(std::numbers::pi / 2).margin(1e-6));
    }
    SECTION("clockwise is stored as the equivalent counter-clockwise arc")
    {
        Fixture f;
        f.view.setTool(ToolKind::ArcCenter);
        f.click(0, 0);
        f.click(10, 0);
        f.move(9, -4);
        f.move(7, -7);
        f.move(0, -10);
        f.click(0, -10);
        const auto arcs = f.ofKind(EntityKind::Arc);
        REQUIRE(arcs.size() == 1);
        const ArcShape a = arcShape(f.sketch(), arcs[0]);
        CHECK(a.startAngle == Approx(-std::numbers::pi / 2).margin(1e-6));
        CHECK(a.sweep == Approx(std::numbers::pi / 2).margin(1e-6));
    }
}

TEST_CASE("Three-point arc passes through the third point", "[tools][arc]")
{
    for (double side : {1.0, -1.0}) {
        Fixture f;
        f.view.setTool(ToolKind::Arc3Point);
        f.click(10, 0);
        f.click(-10, 0);
        f.move(0, 10 * side);
        f.click(0, 10 * side);
        const auto arcs = f.ofKind(EntityKind::Arc);
        REQUIRE(arcs.size() == 1);
        const ArcShape a = arcShape(f.sketch(), arcs[0]);
        CHECK(a.radius == Approx(10.0).margin(1e-6));
        CHECK(a.sweep == Approx(std::numbers::pi).margin(1e-6));
        CHECK(distanceTo(f.sketch(), arcs[0], {0, 10 * side}) < 1e-6);
        CHECK(distanceTo(f.sketch(), arcs[0], {0, -10 * side}) > 1.0);
    }
}

TEST_CASE("Esc abandons a shape in progress", "[tools]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle);
    f.click(0, 0);
    f.key(Qt::Key_Escape);
    f.click(30, 20);  // starts a fresh rectangle rather than finishing the old one
    CHECK(f.sketch().entities().empty());
}

TEST_CASE("Select, delete and undo", "[tools][select]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle);
    f.click(0, 0);
    f.click(30, 20);
    f.view.setTool(ToolKind::Select);

    f.click(15, 0);  // middle of the bottom side
    REQUIRE(f.view.selection().size() == 1);
    const Id bottom = *f.view.selection().begin();
    CHECK(f.sketch().entity(bottom).kind() == EntityKind::Line);

    f.key(Qt::Key_Delete);
    CHECK_FALSE(f.sketch().hasEntity(bottom));
    CHECK(f.ofKind(EntityKind::Line).size() == 3);
    CHECK(f.sketch().constraints().size() == 3);
    CHECK(f.view.selection().empty());

    f.doc.undo();
    CHECK(f.sketch().hasEntity(bottom));
    CHECK(f.ofKind(EntityKind::Line).size() == 4);
    f.doc.redo();
    CHECK(f.ofKind(EntityKind::Line).size() == 3);
}

TEST_CASE("Box selection: window vs crossing", "[tools][select]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(0, 0);
    f.click(10, 0);
    f.key(Qt::Key_Escape);
    f.click(5, 10);
    f.click(40, 10);
    f.key(Qt::Key_Escape);
    f.view.setTool(ToolKind::Select);
    const auto lines = f.ofKind(EntityKind::Line);

    f.drag(-5, -5, 20, 15);  // left to right: window
    CHECK(f.view.selection().contains(lines[0]));
    CHECK_FALSE(f.view.selection().contains(lines[1]));

    f.key(Qt::Key_Escape);  // clear
    CHECK(f.view.selection().empty());
    f.drag(20, 15, -5, -5);  // right to left: crossing
    CHECK(f.view.selection().contains(lines[0]));
    CHECK(f.view.selection().contains(lines[1]));
}

TEST_CASE("Dragging a rectangle corner keeps it rectangular", "[tools][drag]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle);
    f.click(0, 0);
    f.click(30, 20);
    f.view.setTool(ToolKind::Select);

    f.drag(30, 20, 40, 30);

    const auto& s = f.sketch();
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() != EntityKind::Line) {
            continue;
        }
        const Vec2 a = f.pos(s.line(id).p1), b = f.pos(s.line(id).p2);
        const bool horizontal = std::abs(a.y - b.y) < 1e-6;
        const bool vertical = std::abs(a.x - b.x) < 1e-6;
        CHECK((horizontal || vertical));
    }
    // The dragged corner followed the mouse.
    const Id corner = pick(s, {40, 30}, PickOptions{0.5, true, false, {}});
    CHECK(corner != kNoId);

    f.doc.undo();  // the whole drag is one step
    CHECK(pick(f.sketch(), {30, 20}, PickOptions{0.01, true, false, {}}) != kNoId);
}

TEST_CASE("Dragging a circle rim resizes it", "[tools][drag]")
{
    Fixture f;
    f.view.setTool(ToolKind::Circle);
    f.click(0, 0);
    f.click(10, 0);
    f.view.setTool(ToolKind::Select);
    f.drag(10, 0, 15, 0);
    const auto circles = f.ofKind(EntityKind::Circle);
    REQUIRE(circles.size() == 1);
    CHECK(f.sketch().circle(circles[0]).radius == Approx(15.0).margin(1e-3));
}
