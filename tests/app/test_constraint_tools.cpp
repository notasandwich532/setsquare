// End-to-end tests of constraint tools, the dimension tool, inference while drawing, and
// over-constraint rejection.

#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Dimensions.h>
#include <core/Geometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>

#include <cmath>

using namespace core;
using CT = ConstraintType;
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
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y) { QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y)); }
    void clickPx(QPoint p) { QTest::mouseClick(&view, Qt::LeftButton, {}, p); }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    void key(Qt::Key k) { QTest::keyClick(&view, k); }
    const Sketch& sketch() const { return doc.sketch(); }

    Id line(double x1, double y1, double x2, double y2)
    {
        Id id = kNoId;
        doc.modify("line", [&](Sketch& s) { id = s.addLine(x1, y1, x2, y2); });
        return id;
    }

    std::vector<Id> constraintsOfType(CT type) const
    {
        std::vector<Id> out;
        for (const auto& [id, c] : sketch().constraints()) {
            if (c.type == type) {
                out.push_back(id);
            }
        }
        return out;
    }

    QLineEdit* editor() const
    {
        // Closed editors are deleteLater()'d; let that happen so we only see a live one.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return view.findChild<QLineEdit*>("dimensionEditor");
    }

    void typeIntoEditor(const QString& text)
    {
        QLineEdit* e = editor();
        REQUIRE(e != nullptr);
        e->selectAll();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

    double length(Id l) const
    {
        const Point& a = sketch().point(sketch().line(l).p1);
        const Point& b = sketch().point(sketch().line(l).p2);
        return std::hypot(b.x - a.x, b.y - a.y);
    }
};

}  // namespace

// --- geometric constraints ---------------------------------------------------------------------

TEST_CASE("Horizontal/vertical tool picks the axis from the line", "[constraints]")
{
    Fixture f;
    const Id flat = f.line(0, 0, 30, 4);
    const Id steep = f.line(50, 0, 53, 30);
    f.view.setTool(ToolKind::HorizontalVertical);
    f.click(15, 2);
    f.click(51.5, 15);

    REQUIRE(f.constraintsOfType(CT::Horizontal).size() == 1);
    REQUIRE(f.constraintsOfType(CT::Vertical).size() == 1);
    const Point& a = f.sketch().point(f.sketch().line(flat).p1);
    const Point& b = f.sketch().point(f.sketch().line(flat).p2);
    CHECK(a.y == Approx(b.y).margin(1e-9));
    const Point& c = f.sketch().point(f.sketch().line(steep).p1);
    const Point& d = f.sketch().point(f.sketch().line(steep).p2);
    CHECK(c.x == Approx(d.x).margin(1e-9));
}

TEST_CASE("Preselect then pick a constraint tool applies it at once", "[constraints]")
{
    Fixture f;
    const Id a = f.line(0, 0, 30, 2);
    const Id b = f.line(0, 10, 30, 18);
    f.view.setSelection({a, b});
    f.view.setTool(ToolKind::Parallel);

    CHECK(f.constraintsOfType(CT::Parallel).size() == 1);
    CHECK(f.view.selection().empty());
    QCoreApplication::processEvents();  // the tool hands back to Select asynchronously
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Over-constraining is refused with a message", "[constraints]")
{
    Fixture f;
    f.view.setTool(ToolKind::Rectangle);
    f.click(0, 0);
    f.click(30, 20);
    REQUIRE(f.sketch().constraints().size() == 4);

    QSignalSpy messages(&f.view, &SketchView::message);
    f.view.setTool(ToolKind::HorizontalVertical);
    f.click(15, 0);  // bottom side is already horizontal

    CHECK(f.sketch().constraints().size() == 4);
    CHECK(messages.count() == 1);
    CHECK(f.doc.solveResult().redundant.empty());
}

TEST_CASE("Two-pick constraint: point onto a circle", "[constraints]")
{
    Fixture f;
    Id circle = kNoId, point = kNoId;
    f.doc.modify("setup", [&](Sketch& s) {
        circle = s.addCircle(0, 0, 10);
        point = s.addPoint(20, 5);
    });
    f.view.setTool(ToolKind::Coincident);
    f.click(20, 5);
    CHECK(f.sketch().constraints().empty());  // waiting for the second pick
    f.click(0, 10);

    REQUIRE(f.constraintsOfType(CT::Coincident).size() == 1);
    const Point& p = f.sketch().point(point);
    const Point& c = f.sketch().point(f.sketch().circle(circle).center);
    CHECK(std::hypot(p.x - c.x, p.y - c.y) == Approx(f.sketch().circle(circle).radius).margin(1e-7));
}

TEST_CASE("Fix tool toggles a point's lock", "[constraints]")
{
    Fixture f;
    Id p = kNoId;
    f.doc.modify("setup", [&](Sketch& s) { p = s.addPoint(5, 5); });
    f.view.setTool(ToolKind::Fix);
    f.click(5, 5);
    CHECK(f.sketch().entity(p).fixed);
    f.click(5, 5);
    CHECK_FALSE(f.sketch().entity(p).fixed);
}

TEST_CASE("Clicking a constraint symbol selects it; Delete removes it", "[constraints]")
{
    Fixture f;
    const Id l = f.line(0, 0, 30, 0);
    f.doc.addConstraint(CT::Horizontal, {l});
    f.view.setTool(ToolKind::Select);

    // The symbol sits above the line's midpoint, a little along it.
    const QPoint symbol = f.at(15, 0) + QPoint(13, -14);
    f.clickPx(symbol);
    REQUIRE(f.view.selection().size() == 1);
    const Id selected = *f.view.selection().begin();
    CHECK(f.sketch().hasConstraint(selected));

    f.key(Qt::Key_Delete);
    CHECK(f.sketch().constraints().empty());
    CHECK(f.sketch().hasEntity(l));
}

// --- dimensions ---------------------------------------------------------------------------------

TEST_CASE("Dimension a line's length, then type a new value", "[dimension]")
{
    Fixture f;
    const Id l = f.line(0, 0, 30, 0);
    f.doc.modify("fix start", [&](Sketch& s) { s.entity(s.line(l).p1).fixed = true; });

    f.view.setTool(ToolKind::Dimension);
    f.click(15, 0);   // pick the line
    f.move(15, 10);   // alongside it: aligned length
    f.click(15, 10);  // place

    const auto dims = f.constraintsOfType(CT::Distance);
    REQUIRE(dims.size() == 1);
    CHECK(f.sketch().constraint(dims[0]).value == Approx(30.0));
    f.typeIntoEditor("50");
    CHECK(f.length(l) == Approx(50.0).margin(1e-6));
    CHECK(f.editor() == nullptr);
}

TEST_CASE("Placement chooses horizontal / vertical / aligned", "[dimension]")
{
    Fixture f;
    f.line(0, 0, 30, 20);
    f.view.setTool(ToolKind::Dimension);

    f.click(15, 10);
    f.click(25, 40);  // beyond the end, within the x-span: horizontal
    QTest::keyClick(f.editor(), Qt::Key_Escape);
    f.click(15, 10);
    f.click(50, 10);  // within the y-span: vertical
    QTest::keyClick(f.editor(), Qt::Key_Escape);

    const auto h = f.constraintsOfType(CT::HorizontalDistance);
    const auto v = f.constraintsOfType(CT::VerticalDistance);
    REQUIRE(h.size() == 1);
    REQUIRE(v.size() == 1);
    CHECK(f.sketch().constraint(h[0]).value == Approx(30.0));
    CHECK(f.sketch().constraint(v[0]).value == Approx(20.0));
}

TEST_CASE("Dimension a circle (diameter) and the angle between lines", "[dimension]")
{
    Fixture f;
    Id circle = kNoId;
    f.doc.modify("setup", [&](Sketch& s) { circle = s.addCircle(-40, 0, 10); });
    const Id base = f.line(0, 0, 30, 0);
    const Id arm = f.line(0, 0, 20, 20);

    f.view.setTool(ToolKind::Dimension);
    f.click(-30, 0);  // rim
    f.click(-20, 15);
    f.typeIntoEditor("30");
    CHECK(f.sketch().circle(circle).radius == Approx(15.0).margin(1e-6));

    f.click(25, 0);   // base
    f.click(15, 15);  // arm
    f.click(20, 8);   // place
    const auto angles = f.constraintsOfType(CT::Angle);
    REQUIRE(angles.size() == 1);
    CHECK(f.sketch().constraint(angles[0]).value == Approx(45.0));
    (void)base;
    (void)arm;
}

TEST_CASE("Double-click a dimension to edit; impossible values are refused", "[dimension]")
{
    Fixture f;
    const Id l = f.line(0, 0, 20, 0);
    f.doc.modify("fix", [&](Sketch& s) { s.entity(s.line(l).p1).fixed = true; });
    const Id d = f.doc.addConstraint(CT::Distance, {l}, 20.0, nullptr, 0.0, 10.0);  // label at (10, 10)
    REQUIRE(d != kNoId);

    f.view.setTool(ToolKind::Select);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(10, 10));
    REQUIRE(f.editor() != nullptr);

    QSignalSpy messages(&f.view, &SketchView::message);
    f.typeIntoEditor("-5");
    CHECK(messages.count() == 1);
    CHECK(f.sketch().constraint(d).value == Approx(20.0));
}

TEST_CASE("Dragging a dimension label moves only the label", "[dimension]")
{
    Fixture f;
    const Id l = f.line(0, 0, 20, 0);
    const Id d = f.doc.addConstraint(CT::Distance, {l}, 20.0, nullptr, 0.0, 10.0);
    f.view.setTool(ToolKind::Select);

    QTest::mousePress(&f.view, Qt::LeftButton, {}, f.at(10, 10));
    for (int i = 1; i <= 5; ++i) {
        f.move(10, 10 + i * 2);
    }
    QTest::mouseRelease(&f.view, Qt::LeftButton, {}, f.at(10, 20));

    const DimensionLayout L = layoutDimension(f.sketch(), f.sketch().constraint(d));
    CHECK(L.label.y == Approx(20.0).margin(0.3));
    CHECK(f.length(l) == Approx(20.0));
    f.doc.undo();
    CHECK(layoutDimension(f.sketch(), f.sketch().constraint(d)).label.y == Approx(10.0).margin(0.3));
}

TEST_CASE("A fully dimensioned bracket is fully constrained", "[dimension][showcase]")
{
    Fixture f;
    // An L-shaped outline with a hole, drawn with the tools.
    f.view.setTool(ToolKind::Line);
    for (auto [x, y] : std::vector<std::pair<double, double>>{{-40, -30}, {20, -30}, {20, -10}, {-20, -10},
                                                              {-20, 30}, {-40, 30}, {-40, -30}}) {
        f.click(x, y);
    }
    f.view.setTool(ToolKind::Circle);
    f.click(-30, 15);
    f.click(-25, 15);
    REQUIRE(f.constraintsOfType(CT::Horizontal).size() == 3);
    REQUIRE(f.constraintsOfType(CT::Vertical).size() == 3);

    f.view.setTool(ToolKind::Fix);
    f.click(-40, -30);

    auto dimension = [&](std::vector<std::pair<double, double>> picks, std::pair<double, double> place) {
        f.view.setTool(ToolKind::Dimension);
        for (auto [x, y] : picks) {
            f.click(x, y);
        }
        f.move(place.first, place.second);
        f.click(place.first, place.second);
        QTest::keyClick(f.editor(), Qt::Key_Escape);  // keep the measured value
    };
    dimension({{-10, -30}}, {-10, -38});         // bottom width
    dimension({{20, -20}}, {28, -20});           // right height
    dimension({{-30, 30}}, {-30, 37});           // top width
    dimension({{-40, 5}}, {-48, 5});             // left height (off the X axis)
    dimension({{-25, 15}}, {-15, 22});           // hole diameter
    dimension({{-30, 15}, {-40, 15}}, {-35, 20});  // hole centre to left edge
    dimension({{-30, 15}, {-30, 30}}, {-25, 25});  // hole centre to top edge

    f.view.setTool(ToolKind::Select);
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.dof == 0);
    CHECK(r.redundant.empty());
}

// --- inference while drawing -------------------------------------------------------------------

TEST_CASE("Nearly horizontal and vertical lines get H/V constraints", "[inference]")
{
    Fixture f;
    f.view.setTool(ToolKind::Line);
    f.click(10, 10);  // (away from the axes, which would snap the ends onto them instead)
    f.move(30, 10.5);
    f.click(30, 10.5);  // ~1.4 degrees off: snaps horizontal
    f.move(30.5, 30);
    f.click(30.5, 30);  // snaps vertical
    f.move(15, 45);
    f.click(15, 45);  // clearly diagonal: nothing
    f.key(Qt::Key_Escape);

    CHECK(f.constraintsOfType(CT::Horizontal).size() == 1);
    CHECK(f.constraintsOfType(CT::Vertical).size() == 1);
    CHECK(f.sketch().constraints().size() == 2);
}

TEST_CASE("Snapping to a midpoint or onto a curve adds the constraint", "[inference]")
{
    Fixture f;
    const Id base = f.line(0, 0, 40, 0);
    Id circle = kNoId;
    f.doc.modify("circle", [&](Sketch& s) { circle = s.addCircle(60, 30, 10); });

    f.view.setTool(ToolKind::Line);
    f.click(20.3, 0.2);  // near the midpoint of base
    f.click(60, 40);     // top of the circle's rim
    f.key(Qt::Key_Escape);

    const auto mids = f.constraintsOfType(CT::Midpoint);
    REQUIRE(mids.size() == 1);
    CHECK(f.sketch().constraint(mids[0]).refs[1] == base);
    const auto onCurve = f.constraintsOfType(CT::Coincident);
    REQUIRE(onCurve.size() == 1);
    CHECK(f.sketch().constraint(onCurve[0]).refs[1] == circle);
}

TEST_CASE("Constraints apply to a box selection of several curves", "[constraints][selection]")
{
    Fixture f;
    std::vector<Id> lines;
    for (int i = 0; i < 4; ++i) {
        lines.push_back(f.line(10.0 * i, 0, 10.0 * i + 5 + i, 3));  // all a bit different
    }
    // A window selection takes the lines and their endpoints.
    f.view.setTool(ToolKind::Select);
    QTest::mousePress(&f.view, Qt::LeftButton, {}, f.at(-5, 10));
    QTest::mouseMove(&f.view, f.at(20, 0));
    QTest::mouseMove(&f.view, f.at(60, -10));
    QTest::mouseRelease(&f.view, Qt::LeftButton, {}, f.at(60, -10));
    REQUIRE(f.view.selection().size() == 12);

    f.view.setTool(ToolKind::Equal);
    QCoreApplication::processEvents();  // (it hands back to Select afterwards)
    CHECK(f.constraintsOfType(CT::Equal).size() == 3);
    auto length = [&](Id l) {
        const Line& g = f.sketch().line(l);
        const Point& a = f.sketch().point(g.p1);
        const Point& b = f.sketch().point(g.p2);
        return std::hypot(b.x - a.x, b.y - a.y);
    };
    for (Id l : lines) {
        CHECK(length(l) == Approx(length(lines[0])));
    }
    CHECK(f.view.toolKind() == ToolKind::Select);
    f.doc.undo();  // one step for all of them
    CHECK(f.constraintsOfType(CT::Equal).empty());

    // Horizontal/vertical: each selected line.
    f.view.setSelection({lines[0], lines[1], lines[2]});
    f.view.setTool(ToolKind::HorizontalVertical);
    QCoreApplication::processEvents();
    CHECK(f.constraintsOfType(CT::Horizontal).size() == 3);
}

TEST_CASE("Points onto a curve, from a selection", "[constraints][selection]")
{
    Fixture f;
    const Id l = f.line(0, 0, 40, 0);
    Id p = kNoId, q = kNoId;
    f.doc.modify("points", [&](Sketch& s) {
        p = s.addPoint(10, 3);
        q = s.addPoint(30, -2);
    });
    f.view.setSelection({l, p, q});
    f.view.setTool(ToolKind::Coincident);
    QCoreApplication::processEvents();
    CHECK(f.constraintsOfType(CT::Coincident).size() == 2);
    for (Id point : {p, q}) {  // on the line (which may have moved to meet them)
        const Point& at = f.sketch().point(point);
        CHECK(distanceTo(f.sketch(), l, {at.x, at.y}) == Approx(0.0).margin(1e-6));
    }
}

TEST_CASE("Selecting a constraint shows its geometry in pink", "[constraints][selection]")
{
    Fixture f;
    const Id l = f.line(-20, 10, 20, 12);
    Id k = kNoId;
    f.doc.modify("horizontal", [&](Sketch& s) { k = s.addConstraint(CT::Horizontal, {l}); });
    auto pinkPixels = [&] {
        const QImage image = f.view.grab().toImage();
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                count += c.red() > 230 && c.green() < 90 && c.blue() > 180;
            }
        }
        return count;
    };
    CHECK(pinkPixels() == 0);
    f.view.setSelection({k});
    CHECK(pinkPixels() > 100);  // the whole line
}

TEST_CASE("Selecting one of a chain of equal constraints shows the whole chain", "[constraints][selection]")
{
    Fixture f;
    const Id a = f.line(-30, 20, -10, 20);
    const Id b = f.line(-5, 0, 15, 0);
    const Id c = f.line(20, -20, 40, -20);
    const Id other = f.line(-30, -30, -10, -30);  // equal to nothing
    Id first = kNoId;
    f.doc.modify("equal", [&](Sketch& s) {
        first = s.addConstraint(CT::Equal, {a, b});
        s.addConstraint(CT::Equal, {b, c});
    });
    f.view.setSelection({first});
    const QImage image = f.view.grab().toImage();
    auto pinkNear = [&](double x, double y) {
        const QPoint p = f.at(x, y);
        for (int dy = -2; dy <= 2; ++dy) {
            const QColor col = image.pixelColor(p.x(), p.y() + dy);
            if (col.red() > 230 && col.green() < 90 && col.blue() > 180) {
                return true;
            }
        }
        return false;
    };
    CHECK(pinkNear(-20, 20));
    CHECK(pinkNear(5, 0));
    CHECK(pinkNear(30, -20));  // only equal through b, but part of the chain
    CHECK_FALSE(pinkNear(-20, -30));
    (void)other;
}

TEST_CASE("Ratio: pick two lines, type how many times the second the first is; edit it later", "[constraints][ratio]")
{
    Fixture f;
    const Id a = f.line(0, 0, 30, 0);
    const Id b = f.line(0, 10, 20, 10);
    // Pin each line's start and direction, so only their lengths can change.
    f.doc.modify("pin", [&](Sketch& s) {
        for (Id l : {a, b}) {
            s.entity(s.line(l).p1).fixed = true;
            s.addConstraint(CT::Horizontal, {l});
        }
    });
    f.view.setTool(ToolKind::Ratio);
    f.click(15, 0);
    f.click(10, 10);
    QCoreApplication::processEvents();  // the value box opens once the pick is done
    const auto made = f.constraintsOfType(CT::Ratio);
    REQUIRE(made.size() == 1);
    CHECK(f.sketch().constraint(made[0]).value == Approx(1.5));  // as drawn: nothing jumped
    CHECK(f.length(a) == Approx(30.0));
    f.typeIntoEditor("2");
    CHECK(f.sketch().constraint(made[0]).value == Approx(2.0));
    CHECK(f.length(a) == Approx(2.0 * f.length(b)).margin(1e-7));

    // Later: double-click its symbol to change it.
    f.view.setTool(ToolKind::Select);
    QPoint symbol(-1, -1);
    for (int dy = -40; dy <= 40 && symbol.x() < 0; dy += 2) {
        for (int dx = -60; dx <= 60; dx += 2) {
            const QPoint p = f.at(f.length(a) / 2, 0) + QPoint(dx, dy);
            if (f.view.pickAt(QPointF(p)) == made[0]) {
                symbol = p;
                break;
            }
        }
    }
    REQUIRE(symbol.x() >= 0);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, symbol);
    f.typeIntoEditor("0.5");
    CHECK(f.length(a) == Approx(0.5 * f.length(b)).margin(1e-7));
}
TEST_CASE("Number inputs take sums: the dimension box, panels and counts", "[constraints][expression]")
{
    Fixture f;
    // A dimension typed as a sum.
    const Id l = f.line(0, 0, 30, 0);
    Id d = kNoId;
    f.doc.modify("dim", [&](Sketch& s) {
        s.entity(s.line(l).p1).fixed = true;
        s.addConstraint(CT::Horizontal, {l});
        d = s.addConstraint(CT::Distance, {l}, 30.0);
    });
    f.view.editDimension(d);
    f.typeIntoEditor("(100 - 2*15) / 2");
    CHECK(f.sketch().constraint(d).value == Approx(35.0));
    CHECK(f.length(l) == Approx(35.0));

    // Something that doesn't work out is refused, saying why; the value stays.
    QStringList messages;
    QObject::connect(&f.view, &SketchView::message, [&](const QString& m) { messages << m; });
    f.view.editDimension(d);
    f.typeIntoEditor("35/0");
    CHECK(f.sketch().constraint(d).value == Approx(35.0));
    REQUIRE(!messages.isEmpty());
    CHECK(messages.last().contains("divide by zero"));

    // A panel's number box: worked out on Enter, shown as the result.
    f.view.setTool(ToolKind::Fillet);
    QCoreApplication::processEvents();
    auto* radius = f.view.findChild<QDoubleSpinBox*>("radius");
    REQUIRE(radius != nullptr);
    radius->selectAll();
    QTest::keyClicks(radius, "2*3+0.5");
    QTest::keyClick(radius, Qt::Key_Return);
    CHECK(radius->value() == Approx(6.5));
    CHECK(radius->text().startsWith("6.5"));  // shown as the result
    CHECK(f.view.modifyOptions().filletRadius == Approx(6.5));
    // Unreadable: Enter puts it back as it was before typing (not the "2" it passed through).
    radius->selectAll();
    QTest::keyClicks(radius, "2*(");
    QTest::keyClick(radius, Qt::Key_Return);
    CHECK(radius->value() == Approx(6.5));
    CHECK(f.view.modifyOptions().filletRadius == Approx(6.5));

    // A count: whole results only.
    f.view.setTool(ToolKind::Divide);
    QCoreApplication::processEvents();
    auto* points = f.view.findChild<QSpinBox*>("points");
    REQUIRE(points != nullptr);
    points->selectAll();
    QTest::keyClicks(points, "12/4");
    QTest::keyClick(points, Qt::Key_Return);
    CHECK(points->value() == 3);
}